#include "diagnostics_history.h"

#include "decode.h"
#include "diagnostics_logic.h"
#include "history_config.h"
#include "version.h"
#include "webfunctions.h"
#include "htmlcode.h"
#include "zone1_heat_semantics.h"
#include "heating_curve_shift.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if HEISHAMON_SD_HISTORY_ENABLED
#include <SD.h>
#include <SPI.h>
#endif

#if defined(ESP32)
#include <WiFi.h>
#include <ETH.h>
#include <esp_system.h>
#endif

extern char actData[];
extern unsigned long lastHeatpumpDataAt;
extern PubSubClient mqtt_client;
extern settingsStruct heishamonSettings;
extern void log_message(char *string);
extern String getDataValue(char *data, unsigned int topicNumber);
extern void customFeaturesAppendExternalSensorDiagnostics(JsonArray array);
extern void customFeaturesReadExternalSensorHistory(float *values, bool *valid,
  size_t maxValues);
extern bool customFeaturesReadExternalElectricalPower(uint8_t sourceId,
  float *value, uint32_t *ageSeconds);

namespace {

constexpr uint8_t ROUTE_DIAGNOSTICS = 27;
constexpr uint8_t ROUTE_DIAGNOSTICS_API = 28;
constexpr uint8_t ROUTE_HISTORY = 29;
constexpr uint8_t ROUTE_HISTORY_STATUS = 30;
constexpr uint8_t ROUTE_HISTORY_API = 31;
constexpr uint8_t ROUTE_EVENTS_API = 32;
constexpr uint8_t ROUTE_HISTORY_COMMAND = 33;
constexpr uint8_t ROUTE_CYCLES_API = 34;

constexpr uint8_t TOP_HEATPUMP_STATE = 0;
constexpr uint8_t TOP_FLOW = 1;
constexpr uint8_t TOP_FORCE_DHW = 2;
constexpr uint8_t TOP_OPERATION_MODE = 4;
constexpr uint8_t TOP_INLET = 5;
constexpr uint8_t TOP_OUTLET = 6;
constexpr uint8_t TOP_TARGET = 7;
constexpr uint8_t TOP_COMPRESSOR_HZ = 8;
constexpr uint8_t TOP_DHW_TARGET = 9;
constexpr uint8_t TOP_DHW_TEMP = 10;
constexpr uint8_t TOP_OPERATING_HOURS = 11;
constexpr uint8_t TOP_OPERATION_COUNTER = 12;
constexpr uint8_t TOP_OUTSIDE = 14;
constexpr uint8_t TOP_VALVE = 20;
constexpr uint8_t TOP_DEFROST = 26;
constexpr uint8_t TOP_ERROR = 44;
constexpr uint8_t TOP_DHW_PRODUCTION = 40;
constexpr uint8_t TOP_DHW_CONSUMPTION = 41;
constexpr uint8_t TOP_HEAT_POWER_PRODUCTION = 15;
constexpr uint8_t TOP_HEAT_POWER_CONSUMPTION = 16;
constexpr uint8_t TOP_HEX_OUTLET = 49;
constexpr uint8_t TOP_DISCHARGE = 50;
constexpr uint8_t TOP_INSIDE_PIPE = 51;
constexpr uint8_t TOP_DEFROST_TEMP = 52;
constexpr uint8_t TOP_EVA_OUTLET = 53;
constexpr uint8_t TOP_BYPASS_OUTLET = 54;
constexpr uint8_t TOP_IPM = 55;
constexpr uint8_t TOP_FAN1 = 62;
constexpr uint8_t TOP_FAN2 = 63;
constexpr uint8_t TOP_HIGH_PRESSURE = 64;
constexpr uint8_t TOP_PUMP_SPEED = 65;
constexpr uint8_t TOP_LOW_PRESSURE = 66;
constexpr uint8_t TOP_STERILIZATION = 69;
constexpr uint8_t TOP_STERILIZATION_TEMP = 70;
constexpr uint8_t TOP_STERILIZATION_MAX = 71;
constexpr uint8_t TOP_HEATING_MODE = 76;
constexpr uint8_t TOP_HEATING_OFF_OUTSIDE = 77;
constexpr uint8_t TOP_ROOM_TEMP = 56;
constexpr uint8_t TOP_LIQUID_TYPE = 107;
// TOP16 is decoded by HeishaMon as instantaneous electrical power in watts.
// It is quantized, so values below this threshold are not useful for COP.
constexpr float COP_MIN_ELECTRICAL_POWER_W = 100.0f;

constexpr uint8_t SAMPLE_FLAG_COMPRESSOR = 0x01;
constexpr uint8_t SAMPLE_FLAG_HEATPUMP = 0x02;
constexpr uint8_t SAMPLE_FLAG_DHW = 0x04;
constexpr uint8_t SAMPLE_FLAG_DEFROST = 0x08;
constexpr uint8_t SAMPLE_FLAG_TIME_VALID = 0x10;
constexpr uint8_t SAMPLE_FLAG_EXTERNAL_ELECTRICAL = 0x20;

struct HistoryRequest {
  uint32_t rangeSeconds;
  uint16_t maxPoints;
  char response[128];
};

struct CycleState {
  bool initialized = false;
  bool compressorRunning = false;
  bool heatpumpOn = false;
  bool dhwActive = false;
  bool defrostActive = false;
  int valve = -1;
  int operationMode = -1;
  bool errorActive = false;
  uint8_t zone1RequestSemantic = ZONE1_HEAT_SEMANTIC_UNKNOWN;
  unsigned long compressorStartedAt = 0;
  uint32_t compressorStartTimestamp = 0;
  uint32_t compressorStartSequence = 0;
  unsigned long previousRunSeconds = 0;
  uint32_t startsSinceBoot = 0;
  uint64_t totalRunSeconds = 0;
};

struct SdState {
  bool supported = HEISHAMON_SD_HISTORY_ENABLED != 0;
  bool present = false;
  bool filesystemOk = false;
  bool active = false;
  uint64_t capacity = 0;
  uint64_t freeBytes = 0;
  unsigned long lastWriteAt = 0;
  char lastError[80] = "None";
};

static HistorySample samples[HEISHAMON_HISTORY_MAX_SAMPLES];
static HistoryEvent events[HEISHAMON_HISTORY_MAX_EVENTS];
static uint16_t sampleStart = 0;
static uint16_t sampleCount = 0;
static uint16_t eventStart = 0;
static uint16_t eventCount = 0;
static uint16_t sampleIntervalSeconds = HEISHAMON_HISTORY_DEFAULT_INTERVAL_SECONDS;
static uint16_t sdRetentionDays = HEISHAMON_HISTORY_SD_RETENTION_DAYS;
static float heatingDegreeDayBase = 18.0f;
// 0 selects Panasonic TOP power values; non-zero selects an external MQTT
// sensor configured with the electrical-power role.
static uint8_t electricalSourceId = 0;
static unsigned long lastSampleAt = 0;
static unsigned long lastSdFlushAt = 0;
static uint32_t sampleSequence = 0;
static uint32_t sdFlushedSequence = 0;
static uint32_t eventSequence = 0;
static uint32_t sdFlushedEventSequence = 0;
static uint32_t sdLastRetentionDay = 0;
static CycleState cycle;
static SdState sdState;
static bool communicationStateKnown = false;
static bool previousCommunicationState = false;
static bool mqttStateKnown = false;
static bool previousMqttState = false;
static void finalizeCycle(uint32_t stopTimestamp);

static bool frameHeaderValid() {
  return actData[0] == 0x71 && actData[1] == 0xC8 &&
    actData[2] == 0x01 && actData[3] == 0x10;
}

static unsigned long maximumDataAgeMillis() {
  unsigned long configured = (unsigned long)heishamonSettings.waitTime * 4000UL;
  return configured < 60000UL ? 60000UL : configured;
}

static bool dataFresh() {
  return frameHeaderValid() && lastHeatpumpDataAt != 0 &&
    (unsigned long)(millis() - lastHeatpumpDataAt) <= maximumDataAgeMillis();
}

static bool parseFinite(const String &input, float &value) {
  if (input.length() == 0) return false;
  const char *start = input.c_str();
  char *end = nullptr;
  value = strtof(start, &end);
  return end != start && *end == '\0' && isfinite(value);
}

static bool readTopic(uint8_t topic, float &value) {
  if (topic >= NUMBER_OF_TOPICS || !frameHeaderValid()) return false;
  return parseFinite(getDataValue(actData, topic), value);
}

static bool readTemperature(uint8_t topic, float &value) {
  if (!readTopic(topic, value)) return false;
  return diagnosticsValidTemperature(value);
}

static bool readNonNegative(uint8_t topic, float &value) {
  if (!readTopic(topic, value)) return false;
  return value >= 0.0f && value < 100000.0f;
}

static uint16_t scaledUnsigned(float value, float scale);

static bool readPowerWatts(uint8_t topic, uint16_t &watts) {
  float value = 0;
  if (!readNonNegative(topic, value) || value >= 65535.0f) return false;
  watts = scaledUnsigned(value, 1.0f);
  return true;
}

static bool readError(String &value) {
  if (!frameHeaderValid()) return false;
  value = getDataValue(actData, TOP_ERROR);
  return value.length() > 0;
}

static bool validClock(time_t *nowOut = nullptr) {
  time_t now = time(nullptr);
  struct tm local = {};
  bool valid = now > 0 && localtime_r(&now, &local) != nullptr &&
    local.tm_year + 1900 >= 2024;
  if (valid && nowOut != nullptr) *nowOut = now;
  return valid;
}

static uint32_t currentTimestamp(bool *validOut = nullptr) {
  time_t now = 0;
  bool valid = validClock(&now);
  if (validOut != nullptr) *validOut = valid;
  return valid ? (uint32_t)now : (uint32_t)(millis() / 1000UL);
}

static int16_t scaledSigned(float value, float scale) {
  long scaled = lroundf(value * scale);
  if (scaled < -32768L) scaled = -32768L;
  if (scaled > 32767L) scaled = 32767L;
  return (int16_t)scaled;
}

static uint16_t scaledUnsigned(float value, float scale) {
  long scaled = lroundf(value * scale);
  if (scaled < 0) scaled = 0;
  if (scaled > 65535L) scaled = 65535L;
  return (uint16_t)scaled;
}

static void appendText(struct webserver_t *client, const char *text) {
  webserver_send_content(client, (char *)text, strlen(text));
}

static void appendFmt(struct webserver_t *client, const char *format, ...) {
  char buffer[640];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  appendText(client, buffer);
}

static const char *eventTypeName(HistoryEventType type) {
  switch (type) {
    case HISTORY_EVENT_COMPRESSOR_START: return "compressor_start";
    case HISTORY_EVENT_COMPRESSOR_STOP: return "compressor_stop";
    case HISTORY_EVENT_HEATPUMP_ON: return "heatpump_on";
    case HISTORY_EVENT_HEATPUMP_OFF: return "heatpump_off";
    case HISTORY_EVENT_DHW_START: return "dhw_start";
    case HISTORY_EVENT_DHW_STOP: return "dhw_stop";
    case HISTORY_EVENT_DEFROST_START: return "defrost_start";
    case HISTORY_EVENT_DEFROST_STOP: return "defrost_stop";
    case HISTORY_EVENT_VALVE_CHANGED: return "valve_changed";
    case HISTORY_EVENT_ERROR_APPEARED: return "error_appeared";
    case HISTORY_EVENT_ERROR_CLEARED: return "error_cleared";
    case HISTORY_EVENT_SCHEDULER: return "scheduler";
    case HISTORY_EVENT_SMART_DHW: return "smart_dhw";
    case HISTORY_EVENT_COMMUNICATION: return "communication";
    case HISTORY_EVENT_MQTT: return "mqtt";
    case HISTORY_EVENT_OPERATION_MODE_CHANGED: return "operation_mode_changed";
    case HISTORY_EVENT_ZONE1_SEMANTIC_CHANGED: return "zone1_semantic_changed";
    default: return "unknown";
  }
}

static void logHistoryEvent(HistoryEventType type, const char *message) {
  char line[128];
  snprintf(line, sizeof(line), "[HISTORY] %s: %s", eventTypeName(type), message);
  log_message(line);
}

static void addEvent(HistoryEventType type, const char *message, int32_t value = 0) {
  uint16_t index;
  if (eventCount < HEISHAMON_HISTORY_MAX_EVENTS) {
    index = (uint16_t)((eventStart + eventCount) % HEISHAMON_HISTORY_MAX_EVENTS);
    eventCount++;
  } else {
    index = eventStart;
    eventStart = (uint16_t)((eventStart + 1) % HEISHAMON_HISTORY_MAX_EVENTS);
  }
  HistoryEvent &event = events[index];
  bool timestampValid = false;
  event.timestamp = currentTimestamp(&timestampValid);
  event.uptimeSeconds = (uint32_t)(millis() / 1000UL);
  event.sequence = ++eventSequence;
  event.value = value;
  event.type = type;
  event.timeValid = timestampValid ? 1 : 0;
  snprintf(event.message, sizeof(event.message), "%s", message == nullptr ? "" : message);
  logHistoryEvent(type, event.message);
}

static bool currentErrorActive() {
  String error;
  if (!readError(error)) return false;
  return error != "No error" && error != "No Error" && error != "0";
}

static bool currentDhwActive() {
  float force = 0;
  float valve = 0;
  bool forceValid = readTopic(TOP_FORCE_DHW, force);
  bool valveValid = readTopic(TOP_VALVE, valve);
  return (forceValid && lroundf(force) != 0) || (valveValid && lroundf(valve) == 1);
}

static void updateCycleState() {
  if (!dataFresh()) return;
  float value = 0;
  float compressor = 0;
  bool compressorValid = readNonNegative(TOP_COMPRESSOR_HZ, compressor);
  bool compressorRunning = compressorValid && compressor > 0.5f;
  bool heatpumpOn = readTopic(TOP_HEATPUMP_STATE, value) && lroundf(value) != 0;
  bool dhwActive = currentDhwActive();
  bool defrostActive = readTopic(TOP_DEFROST, value) && lroundf(value) != 0;
  int valve = readTopic(TOP_VALVE, value) ? (int)lroundf(value) : -1;
  bool errorActive = currentErrorActive();
  int operationMode = readTopic(TOP_OPERATION_MODE, value) ? (int)lroundf(value) : -1;
  Zone1HeatRequestSemantic zone1Semantic;
  uint8_t currentZone1Semantic = ZONE1_HEAT_SEMANTIC_UNKNOWN;
  if (resolveZone1HeatRequestSemantic(actData, heishamonSettings.wpHeatMin,
      heishamonSettings.wpHeatMax, &zone1Semantic)) {
    currentZone1Semantic = (uint8_t)zone1Semantic.type;
  }

  if (!cycle.initialized) {
    cycle.initialized = true;
    cycle.compressorRunning = compressorRunning;
    cycle.heatpumpOn = heatpumpOn;
    cycle.dhwActive = dhwActive;
    cycle.defrostActive = defrostActive;
    cycle.valve = valve;
    cycle.operationMode = operationMode;
    cycle.errorActive = errorActive;
    cycle.zone1RequestSemantic = currentZone1Semantic;
    if (compressorRunning) {
      cycle.compressorStartedAt = millis();
      cycle.compressorStartTimestamp = currentTimestamp();
      cycle.compressorStartSequence = sampleSequence + 1;
    }
    return;
  }

  if (compressorRunning != cycle.compressorRunning) {
    if (compressorRunning) {
      cycle.compressorStartedAt = millis();
      cycle.compressorStartTimestamp = currentTimestamp();
      cycle.compressorStartSequence = sampleSequence + 1;
      cycle.startsSinceBoot++;
      addEvent(HISTORY_EVENT_COMPRESSOR_START, "Compressor started");
    } else {
      finalizeCycle(currentTimestamp());
      unsigned long runSeconds = cycle.compressorStartedAt == 0 ? 0 :
        (unsigned long)(millis() - cycle.compressorStartedAt) / 1000UL;
      cycle.previousRunSeconds = runSeconds;
      cycle.totalRunSeconds += runSeconds;
      addEvent(HISTORY_EVENT_COMPRESSOR_STOP, "Compressor stopped", (int32_t)runSeconds);
    }
  }
  if (heatpumpOn != cycle.heatpumpOn) {
    addEvent(heatpumpOn ? HISTORY_EVENT_HEATPUMP_ON : HISTORY_EVENT_HEATPUMP_OFF,
      heatpumpOn ? "Heat pump on" : "Heat pump off");
  }
  if (dhwActive != cycle.dhwActive) {
    addEvent(dhwActive ? HISTORY_EVENT_DHW_START : HISTORY_EVENT_DHW_STOP,
      dhwActive ? "DHW operation started" : "DHW operation stopped");
  }
  if (defrostActive != cycle.defrostActive) {
    addEvent(defrostActive ? HISTORY_EVENT_DEFROST_START : HISTORY_EVENT_DEFROST_STOP,
      defrostActive ? "Defrost started" : "Defrost stopped");
  }
  if (valve >= 0 && cycle.valve >= 0 && valve != cycle.valve) {
    addEvent(HISTORY_EVENT_VALVE_CHANGED, "Three-way valve changed", valve);
  }
  if (operationMode >= 0 && cycle.operationMode >= 0 && operationMode != cycle.operationMode) {
    addEvent(HISTORY_EVENT_OPERATION_MODE_CHANGED, "Operating mode changed", operationMode);
  }
  if (currentZone1Semantic != cycle.zone1RequestSemantic) {
    char message[48];
    snprintf(message, sizeof(message), "Z1: %s>%s",
      zone1HeatRequestSemanticName((Zone1HeatRequestSemanticType)cycle.zone1RequestSemantic),
      zone1HeatRequestSemanticName((Zone1HeatRequestSemanticType)currentZone1Semantic));
    addEvent(HISTORY_EVENT_ZONE1_SEMANTIC_CHANGED, message, currentZone1Semantic);
  }
  if (errorActive != cycle.errorActive) {
    addEvent(errorActive ? HISTORY_EVENT_ERROR_APPEARED : HISTORY_EVENT_ERROR_CLEARED,
      errorActive ? "Heat pump error appeared" : "Heat pump error cleared");
  }
  cycle.compressorRunning = compressorRunning;
  cycle.heatpumpOn = heatpumpOn;
  cycle.dhwActive = dhwActive;
  cycle.defrostActive = defrostActive;
  cycle.valve = valve;
  cycle.operationMode = operationMode;
  cycle.errorActive = errorActive;
  cycle.zone1RequestSemantic = currentZone1Semantic;
}

static bool makeSample(HistorySample &sample) {
  if (!dataFresh()) return false;
  memset(&sample, 0, sizeof(sample));
  bool timestampValid = false;
  sample.timestamp = currentTimestamp(&timestampValid);
  sample.uptimeSeconds = (uint32_t)(millis() / 1000UL);
  float value = 0;
  if (readTemperature(TOP_OUTSIDE, value)) {
    sample.outsideTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_OUTSIDE;
  }
  if (readTemperature(TOP_INLET, value)) {
    sample.inletTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_INLET;
  }
  if (readTemperature(TOP_OUTLET, value)) {
    sample.outletTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_OUTLET;
  }
  if (readTemperature(TOP_TARGET, value)) {
    sample.targetTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_TARGET;
  }
  if (readTemperature(TOP_DHW_TEMP, value)) {
    sample.dhwTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_DHW;
  }
  if (readTemperature(TOP_DHW_TARGET, value)) {
    sample.dhwTargetTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_DHW_TARGET;
  }
  if (readTemperature(TOP_ROOM_TEMP, value)) {
    sample.roomTemp10 = scaledSigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_ROOM;
  }
  if (readNonNegative(TOP_FLOW, value)) {
    sample.flow100 = scaledUnsigned(value, 100.0f);
    sample.validFields |= HISTORY_FIELD_FLOW;
  }
  if (readNonNegative(TOP_COMPRESSOR_HZ, value)) {
    sample.compressorHz10 = scaledUnsigned(value, 10.0f);
    sample.validFields |= HISTORY_FIELD_COMPRESSOR_HZ;
  }
  if (readNonNegative(TOP_PUMP_SPEED, value)) {
    sample.pumpRpm = scaledUnsigned(value, 1.0f);
    sample.validFields |= HISTORY_FIELD_PUMP_RPM;
  }
  uint16_t powerWatts = 0;
  if (readPowerWatts(TOP_HEAT_POWER_PRODUCTION, powerWatts)) {
    sample.heatProductionW = powerWatts;
    sample.validFields |= HISTORY_FIELD_HEAT_PRODUCTION;
  }
  if (readPowerWatts(TOP_HEAT_POWER_CONSUMPTION, powerWatts)) {
    sample.heatConsumptionW = powerWatts;
    sample.validFields |= HISTORY_FIELD_HEAT_CONSUMPTION;
  }
  if (readPowerWatts(TOP_DHW_PRODUCTION, powerWatts)) {
    sample.dhwProductionW = powerWatts;
    sample.validFields |= HISTORY_FIELD_DHW_PRODUCTION;
  }
  if (readPowerWatts(TOP_DHW_CONSUMPTION, powerWatts)) {
    sample.dhwConsumptionW = powerWatts;
    sample.validFields |= HISTORY_FIELD_DHW_CONSUMPTION;
  }
  bool dhwActive = currentDhwActive();
  if (electricalSourceId != 0) {
    float externalPower = 0;
    uint32_t externalAge = UINT32_MAX;
    if (customFeaturesReadExternalElectricalPower(electricalSourceId,
        &externalPower, &externalAge) && externalPower >= 0.0f &&
        externalPower < 65535.0f) {
      sample.electricalPowerW = scaledUnsigned(externalPower, 1.0f);
      sample.validFields |= HISTORY_FIELD_ELECTRICAL_POWER;
      sample.flags |= SAMPLE_FLAG_EXTERNAL_ELECTRICAL;
    }
    (void)externalAge;
  } else if (readPowerWatts(dhwActive ? TOP_DHW_CONSUMPTION : TOP_HEAT_POWER_CONSUMPTION,
      powerWatts)) {
    sample.electricalPowerW = powerWatts;
    sample.validFields |= HISTORY_FIELD_ELECTRICAL_POWER;
  }
  sample.electricalSource = electricalSourceId;
  if (readTopic(TOP_OPERATION_MODE, value)) sample.operatingMode = (uint8_t)max(0L, min(255L, lroundf(value)));
  if (readTopic(TOP_VALVE, value)) sample.valveState = (uint8_t)max(0L, min(255L, lroundf(value)));
  if (readTopic(TOP_HEATPUMP_STATE, value) && lroundf(value) != 0) sample.flags |= SAMPLE_FLAG_HEATPUMP;
  if (readNonNegative(TOP_COMPRESSOR_HZ, value) && value > 0.5f) sample.flags |= SAMPLE_FLAG_COMPRESSOR;
  if (dhwActive) sample.flags |= SAMPLE_FLAG_DHW;
  if (readTopic(TOP_DEFROST, value) && lroundf(value) != 0) sample.flags |= SAMPLE_FLAG_DEFROST;
  Zone1HeatRequestSemantic zone1Semantic;
  float zone1Request = 0;
  if (resolveZone1HeatRequestSemantic(actData, heishamonSettings.wpHeatMin,
      heishamonSettings.wpHeatMax, &zone1Semantic) &&
      zone1Semantic.type != ZONE1_HEAT_CURVE_SHIFT &&
      readZone1HeatRequestRaw(actData, &zone1Request) &&
      isfinite(zone1Request)) {
    sample.zone1RequestValue10 = scaledSigned(zone1Request, 10.0f);
    sample.zone1RequestSemantic = (uint8_t)zone1Semantic.type;
    sample.validFields |= HISTORY_FIELD_ZONE1_REQUEST;
  }
  HeatingCurveShiftStatus curveShift;
  if (heatingCurveShiftGetStatus(&curveShift) && curveShift.available &&
      curveShift.valueValid) {
    sample.heatingCurveShift = curveShift.requestedShift;
    sample.validFields |= HISTORY_FIELD_HEATING_CURVE_SHIFT;
  }
  if (timestampValid) sample.flags |= SAMPLE_FLAG_TIME_VALID;
  sample.operatingState = (sample.flags & SAMPLE_FLAG_DEFROST) ? HISTORY_STATE_DEFROST :
    (sample.flags & SAMPLE_FLAG_COMPRESSOR) ?
      (sample.flags & SAMPLE_FLAG_DHW ? HISTORY_STATE_DHW : HISTORY_STATE_HEATING) :
    (sample.flags & SAMPLE_FLAG_HEATPUMP) ?
      (sample.validFields & HISTORY_FIELD_FLOW) && sample.flow100 > 20 ?
        HISTORY_STATE_CIRCULATION : HISTORY_STATE_STANDBY : HISTORY_STATE_UNKNOWN;

  float inlet = 0, outlet = 0, flow = 0;
  if (readTemperature(TOP_INLET, inlet) && readTemperature(TOP_OUTLET, outlet) &&
      readNonNegative(TOP_FLOW, flow) && flow > 0.2f) {
    float liquid = 0;
    bool water = !readTopic(TOP_LIQUID_TYPE, liquid) || lroundf(liquid) == 0;
    float calculatedPower = 0;
    if (diagnosticsCalculateThermalPower(flow, outlet - inlet, true, true,
        water, calculatedPower)) {
      sample.thermalPower100 = scaledSigned(calculatedPower, 100.0f);
      sample.validFields |= HISTORY_FIELD_THERMAL_POWER;
    }
  }
  float externalValues[HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX];
  bool externalValid[HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX];
  customFeaturesReadExternalSensorHistory(externalValues, externalValid,
    HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX);
  for (uint8_t i = 0; i < HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX; i++) {
    if (externalValid[i]) {
      sample.externalValues[i] = externalValues[i];
      sample.validFields |= (uint32_t)(HISTORY_FIELD_EXTERNAL_0 << i);
    }
  }
  return true;
}

static void storeSample(const HistorySample &sample) {
  uint16_t index = (uint16_t)((sampleStart + sampleCount) % HEISHAMON_HISTORY_MAX_SAMPLES);
  if (sampleCount < HEISHAMON_HISTORY_MAX_SAMPLES) sampleCount++;
  else sampleStart = (uint16_t)((sampleStart + 1) % HEISHAMON_HISTORY_MAX_SAMPLES);
  samples[index] = sample;
}

static uint16_t parseInterval(const char *value) {
  if (value == nullptr || *value == '\0') return 0;
  char *end = nullptr;
  long parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 1 || parsed > 10) return 0;
  return (uint16_t)(parsed * 60);
}

static uint16_t parseRetention(const char *value) {
  if (value == nullptr || *value == '\0') return UINT16_MAX;
  char *end = nullptr;
  long parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 0 || parsed > 90) return UINT16_MAX;
  if (parsed != 0 && parsed != 7 && parsed != 14 && parsed != 30 && parsed != 90) return UINT16_MAX;
  return (uint16_t)parsed;
}

static uint32_t parseRange(const char *value) {
  if (value == nullptr || strcmp(value, "all") == 0) return 0;
  if (strcmp(value, "30m") == 0) return 1800UL;
  if (strcmp(value, "1h") == 0) return 3600UL;
  if (strcmp(value, "3h") == 0) return 10800UL;
  if (strcmp(value, "6h") == 0) return 21600UL;
  if (strcmp(value, "24h") == 0) return 86400UL;
  if (strcmp(value, "7d") == 0) return 604800UL;
  if (strcmp(value, "30d") == 0) return 2592000UL;
  return UINT32_MAX;
}

static uint16_t orderedSampleIndex(uint16_t offset);

static bool sampleInRange(const HistorySample &sample, uint32_t rangeSeconds) {
  if (rangeSeconds == 0) return true;
  bool timeValid = (sample.flags & SAMPLE_FLAG_TIME_VALID) != 0 && validClock();
  uint32_t now = currentTimestamp();
  uint32_t sampleTime = timeValid ? sample.timestamp : sample.uptimeSeconds;
  if (!timeValid) now = (uint32_t)(millis() / 1000UL);
  return now >= sampleTime && now - sampleTime <= rangeSeconds;
}

static bool sampleHasValidTime(const HistorySample &sample) {
  return (sample.flags & SAMPLE_FLAG_TIME_VALID) != 0;
}

static uint32_t sampleTimeSeconds(const HistorySample &sample) {
  return sampleHasValidTime(sample) ? sample.timestamp : sample.uptimeSeconds;
}

// 1 = heating, 2 = DHW. Cooling and unknown modes are deliberately excluded:
// the water-side formula is not a trustworthy cooling COP measurement.
static int efficiencyGroup(const HistorySample &sample) {
  if ((sample.flags & SAMPLE_FLAG_COMPRESSOR) == 0 ||
      (sample.flags & SAMPLE_FLAG_DEFROST) != 0) return 0;
  switch (sample.operatingMode) {
    case 1: // Heat
    case 3: // Auto(heat)
      return (sample.flags & SAMPLE_FLAG_DHW) != 0 ? 2 : 1;
    case 4: // DHW
    case 5: // Heat+DHW
    case 7: // Auto(heat)+DHW
      return (sample.flags & SAMPLE_FLAG_DHW) != 0 ? 2 : 1;
    default:
      return 0;
  }
}

static bool instantaneousCop(const HistorySample &sample, float &cop) {
  if (efficiencyGroup(sample) == 0 ||
      (sample.validFields & HISTORY_FIELD_THERMAL_POWER) == 0 ||
      (sample.validFields & HISTORY_FIELD_ELECTRICAL_POWER) == 0) return false;
  float thermal = sample.thermalPower100 / 100.0f;
  float electrical = sample.electricalPowerW;
  if (!isfinite(thermal) || !isfinite(electrical) || thermal <= 0.1f ||
      electrical < COP_MIN_ELECTRICAL_POWER_W) return false;
  return diagnosticsCalculateEstimatedCop(thermal, electrical / 1000.0f, true,
    true, true, true, COP_MIN_ELECTRICAL_POWER_W / 1000.0f, cop);
}

struct EnergyTotals {
  double thermalKWh = 0.0;
  double electricalKWh = 0.0;
  uint32_t intervals = 0;
};

struct CycleRecord {
  uint32_t startTimestamp;
  uint32_t stopTimestamp;
  uint32_t durationSeconds;
  uint32_t startsSequence;
  uint8_t operatingState;
  float outsideAverage;
  float flowAverage;
  float frequencyAverage;
  float frequencyMaximum;
  double thermalKWh;
  double electricalKWh;
  float cop;
  bool copValid;
};

struct DailySummary {
  bool valid = false;
  uint32_t dayStart = 0;
  double heatingDegreeDays = 0.0;
  EnergyTotals heating;
  EnergyTotals dhw;
  uint32_t compressorSeconds = 0;
  uint32_t compressorStarts = 0;
  float outsideMinimum = NAN;
  float outsideMaximum = NAN;
  double outsideSum = 0.0;
  uint32_t outsideSamples = 0;
};

static CycleRecord cycleRecords[32];
static uint8_t cycleRecordStart = 0;
static uint8_t cycleRecordCount = 0;
static DailySummary dailySummary;
static HistorySample previousDailySample;
static bool previousDailySampleValid = false;

static bool addEnergyPair(const HistorySample &previous, const HistorySample &current,
    int groupFilter, uint32_t lowerTime, uint32_t upperTime, EnergyTotals &totals) {
  if (!sampleHasValidTime(previous) || !sampleHasValidTime(current)) return false;
  uint32_t previousTime = sampleTimeSeconds(previous);
  uint32_t currentTime = sampleTimeSeconds(current);
  if (currentTime <= previousTime || currentTime - previousTime > 600UL ||
      previousTime < lowerTime || currentTime > upperTime) return false;
  int previousGroup = efficiencyGroup(previous);
  int currentGroup = efficiencyGroup(current);
  if (previousGroup == 0 || currentGroup == 0 || previousGroup != currentGroup ||
      (groupFilter != 0 && currentGroup != groupFilter)) return false;
  float previousThermal = previous.thermalPower100 / 100.0f;
  float currentThermal = current.thermalPower100 / 100.0f;
  float previousElectrical = previous.electricalPowerW / 1000.0f;
  float currentElectrical = current.electricalPowerW / 1000.0f;
  bool thermalAdded =
    (previous.validFields & HISTORY_FIELD_THERMAL_POWER) != 0 &&
    (current.validFields & HISTORY_FIELD_THERMAL_POWER) != 0 &&
    previousThermal > 0.1f && currentThermal > 0.1f &&
    diagnosticsIntegratePower(previousThermal, currentThermal,
      currentTime - previousTime, totals.thermalKWh);
  bool electricalAdded =
    (previous.validFields & HISTORY_FIELD_ELECTRICAL_POWER) != 0 &&
    (current.validFields & HISTORY_FIELD_ELECTRICAL_POWER) != 0 &&
    previous.electricalPowerW >= COP_MIN_ELECTRICAL_POWER_W &&
    current.electricalPowerW >= COP_MIN_ELECTRICAL_POWER_W &&
    diagnosticsIntegratePower(previousElectrical, currentElectrical,
      currentTime - previousTime, totals.electricalKWh);
  if (thermalAdded && electricalAdded) totals.intervals++;
  return thermalAdded || electricalAdded;
}

static bool totalsCop(const EnergyTotals &totals, float &cop) {
  if (totals.intervals == 0) return false;
  return diagnosticsCalculateEnergyCop(totals.thermalKWh, totals.electricalKWh, cop);
}

static void persistDailySummary(const DailySummary &summary);
static void persistCycleRecord(const CycleRecord &record);

static void addCycleRecord(const CycleRecord &record) {
  uint8_t index;
  if (cycleRecordCount < 32) {
    index = (uint8_t)((cycleRecordStart + cycleRecordCount) % 32);
    cycleRecordCount++;
  } else {
    index = cycleRecordStart;
    cycleRecordStart = (uint8_t)((cycleRecordStart + 1) % 32);
  }
  cycleRecords[index] = record;
  persistCycleRecord(record);
}

static void finalizeCycle(uint32_t stopTimestamp) {
  if (cycle.compressorStartSequence == 0 || sampleCount == 0) return;
  CycleRecord record = {};
  record.startTimestamp = cycle.compressorStartTimestamp;
  record.stopTimestamp = stopTimestamp;
  record.startsSequence = cycle.compressorStartSequence;
  record.durationSeconds = record.startTimestamp > 0 && stopTimestamp >= record.startTimestamp ?
    stopTimestamp - record.startTimestamp : 0;
  const HistorySample *previous = nullptr;
  double outsideSum = 0.0, flowSum = 0.0, frequencySum = 0.0;
  uint32_t outsideCount = 0, flowCount = 0, frequencyCount = 0;
  bool sawDhw = false;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    const HistorySample &sample = samples[orderedSampleIndex(offset)];
    if (sample.sequence < cycle.compressorStartSequence) continue;
    if ((sample.flags & SAMPLE_FLAG_DHW) != 0) sawDhw = true;
    if ((sample.validFields & HISTORY_FIELD_OUTSIDE) != 0) {
      outsideSum += sample.outsideTemp10 / 10.0f;
      outsideCount++;
    }
    if ((sample.validFields & HISTORY_FIELD_FLOW) != 0) {
      flowSum += sample.flow100 / 100.0f;
      flowCount++;
    }
    if ((sample.validFields & HISTORY_FIELD_COMPRESSOR_HZ) != 0) {
      float hz = sample.compressorHz10 / 10.0f;
      frequencySum += hz;
      frequencyCount++;
      if (frequencyCount == 1 || hz > record.frequencyMaximum) record.frequencyMaximum = hz;
    }
    previous = &sample;
  }
  // Recompute the cycle energy with the final DHW classification. The helper
  // only returns source-based energy and deliberately excludes invalid/defrost
  // intervals; no averaged COP is persisted.
  EnergyTotals totals;
  previous = nullptr;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    const HistorySample &sample = samples[orderedSampleIndex(offset)];
    if (sample.sequence < cycle.compressorStartSequence) continue;
    if (previous != nullptr) addEnergyPair(*previous, sample, 0, 0, UINT32_MAX, totals);
    previous = &sample;
  }
  record.thermalKWh = totals.thermalKWh;
  record.electricalKWh = totals.electricalKWh;
  record.copValid = totalsCop(totals, record.cop);
  record.operatingState = sawDhw ? HISTORY_STATE_DHW : HISTORY_STATE_HEATING;
  record.outsideAverage = outsideCount == 0 ? NAN : (float)(outsideSum / outsideCount);
  record.flowAverage = flowCount == 0 ? NAN : (float)(flowSum / flowCount);
  record.frequencyAverage = frequencyCount == 0 ? NAN : (float)(frequencySum / frequencyCount);
  addCycleRecord(record);
}

static uint32_t localDayStart(uint32_t timestamp) {
  time_t value = (time_t)timestamp;
  struct tm local = {};
  if (localtime_r(&value, &local) == nullptr) return 0;
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  return (uint32_t)mktime(&local);
}

static void resetDailySummary(uint32_t dayStart) {
  dailySummary = DailySummary();
  dailySummary.valid = dayStart != 0;
  dailySummary.dayStart = dayStart;
}

static void updateDailySummary(const HistorySample &sample) {
  if (!sampleHasValidTime(sample)) return;
  uint32_t dayStart = localDayStart(sample.timestamp);
  if (dayStart == 0) return;
  if (!dailySummary.valid) resetDailySummary(dayStart);
  else if (dailySummary.dayStart != dayStart) {
    persistDailySummary(dailySummary);
    resetDailySummary(dayStart);
  }
  if ((sample.validFields & HISTORY_FIELD_OUTSIDE) != 0) {
    float outside = sample.outsideTemp10 / 10.0f;
    dailySummary.outsideSum += outside;
    dailySummary.outsideSamples++;
    if (!isfinite(dailySummary.outsideMinimum) || outside < dailySummary.outsideMinimum) dailySummary.outsideMinimum = outside;
    if (!isfinite(dailySummary.outsideMaximum) || outside > dailySummary.outsideMaximum) dailySummary.outsideMaximum = outside;
  }
  if (previousDailySampleValid && sampleHasValidTime(previousDailySample)) {
    uint32_t previousTime = sampleTimeSeconds(previousDailySample);
    uint32_t currentTime = sampleTimeSeconds(sample);
    if (currentTime > previousTime && currentTime - previousTime <= 600UL &&
        localDayStart(previousTime) == dayStart) {
      if ((previousDailySample.validFields & HISTORY_FIELD_OUTSIDE) != 0 &&
          (sample.validFields & HISTORY_FIELD_OUTSIDE) != 0) {
        dailySummary.heatingDegreeDays += diagnosticsHeatingDegreeDays(heatingDegreeDayBase,
          previousDailySample.outsideTemp10 / 10.0,
          sample.outsideTemp10 / 10.0, currentTime - previousTime);
      }
      addEnergyPair(previousDailySample, sample, 1, dayStart, UINT32_MAX, dailySummary.heating);
      addEnergyPair(previousDailySample, sample, 2, dayStart, UINT32_MAX, dailySummary.dhw);
      if ((sample.flags & SAMPLE_FLAG_COMPRESSOR) != 0) dailySummary.compressorSeconds += currentTime - previousTime;
      if ((sample.flags & SAMPLE_FLAG_COMPRESSOR) != 0 &&
          (previousDailySample.flags & SAMPLE_FLAG_COMPRESSOR) == 0) dailySummary.compressorStarts++;
    }
  }
  previousDailySample = sample;
  previousDailySampleValid = true;
}

static EnergyTotals calculateEnergy(uint16_t firstOffset, uint16_t lastOffset,
    int groupFilter, uint32_t lowerTime, uint32_t upperTime) {
  EnergyTotals totals;
  if (sampleCount < 2 || firstOffset >= sampleCount || lastOffset >= sampleCount ||
      firstOffset >= lastOffset) return totals;
  const HistorySample *previous = &samples[orderedSampleIndex(firstOffset)];
  for (uint16_t offset = firstOffset + 1; offset <= lastOffset; offset++) {
    const HistorySample &current = samples[orderedSampleIndex(offset)];
    addEnergyPair(*previous, current, groupFilter, lowerTime, upperTime, totals);
    previous = &current;
  }
  return totals;
}

static bool latestSampleCop(float &cop) {
  if (sampleCount == 0) return false;
  return instantaneousCop(samples[orderedSampleIndex(sampleCount - 1)], cop);
}

static bool currentCycleCop(float &cop) {
  if (sampleCount < 2) return false;
  uint16_t newest = sampleCount - 1;
  int group = efficiencyGroup(samples[orderedSampleIndex(newest)]);
  if (group == 0) return false;
  uint16_t first = newest;
  while (first > 0) {
    const HistorySample &previous = samples[orderedSampleIndex(first - 1)];
    const HistorySample &current = samples[orderedSampleIndex(first)];
    if (efficiencyGroup(previous) != group ||
        !sampleHasValidTime(previous) || !sampleHasValidTime(current) ||
        sampleTimeSeconds(current) - sampleTimeSeconds(previous) > 600UL) break;
    first--;
  }
  return totalsCop(calculateEnergy(first, newest, group, 0, UINT32_MAX), cop);
}

static bool rangeCop(uint32_t lowerTime, uint32_t upperTime, int group, float &cop) {
  if (sampleCount < 2) return false;
  return totalsCop(calculateEnergy(0, sampleCount - 1, group, lowerTime, upperTime), cop);
}

static void appendJsonFloat(struct webserver_t *client, bool valid, float value) {
  if (valid && isfinite(value)) appendFmt(client, "%.2f", value);
  else appendText(client, "null");
}

static void appendEfficiencySummary(struct webserver_t *client) {
  float currentCop = 0, cycleCop = 0, todayHeatingCop = 0, todayDhwCop = 0;
  float last24Cop = 0;
  bool clockValid = false;
  time_t now = 0;
  clockValid = validClock(&now);
  uint32_t nowSeconds = clockValid ? (uint32_t)now : 0;
  uint32_t midnight = 0;
  if (clockValid) {
    struct tm local = {};
    if (localtime_r(&now, &local) != nullptr) {
      local.tm_hour = 0;
      local.tm_min = 0;
      local.tm_sec = 0;
      midnight = (uint32_t)mktime(&local);
    }
  }
  bool currentValid = latestSampleCop(currentCop);
  bool cycleValid = currentCycleCop(cycleCop);
  bool heatingValid = clockValid && rangeCop(midnight, nowSeconds, 1, todayHeatingCop);
  bool dhwValid = clockValid && rangeCop(midnight, nowSeconds, 2, todayDhwCop);
  bool last24Valid = clockValid && rangeCop(nowSeconds - 86400UL, nowSeconds, 0, last24Cop);
  appendText(client, "{\"currentEstimatedCop\":");
  appendJsonFloat(client, currentValid, currentCop);
  appendText(client, ",\"currentCycleCop\":");
  appendJsonFloat(client, cycleValid, cycleCop);
  appendText(client, ",\"todayHeatingCop\":");
  appendJsonFloat(client, heatingValid, todayHeatingCop);
  appendText(client, ",\"todayDhwCop\":");
  appendJsonFloat(client, dhwValid, todayDhwCop);
  appendText(client, ",\"last24hCop\":");
  appendJsonFloat(client, last24Valid, last24Cop);
  appendText(client, "}");
}

static void appendDailySummaryJson(struct webserver_t *client) {
  float heatingCop = 0, dhwCop = 0;
  bool heatingValid = totalsCop(dailySummary.heating, heatingCop);
  bool dhwValid = totalsCop(dailySummary.dhw, dhwCop);
  float outsideAverage = dailySummary.outsideSamples == 0 ? NAN :
    (float)(dailySummary.outsideSum / dailySummary.outsideSamples);
  appendFmt(client, "{\"valid\":%s,\"dayStart\":%lu,\"heatingDegreeDays\":%.3f,\"compressorSeconds\":%lu,\"compressorStarts\":%lu,\"heatingThermalKWh\":%.4f,\"heatingElectricalKWh\":%.4f,\"heatingCop\":",
    dailySummary.valid ? "true" : "false", (unsigned long)dailySummary.dayStart,
    dailySummary.heatingDegreeDays, (unsigned long)dailySummary.compressorSeconds,
    (unsigned long)dailySummary.compressorStarts, dailySummary.heating.thermalKWh,
    dailySummary.heating.electricalKWh);
  appendJsonFloat(client, heatingValid, heatingCop);
  appendFmt(client, ",\"dhwThermalKWh\":%.4f,\"dhwElectricalKWh\":%.4f,\"dhwCop\":",
    dailySummary.dhw.thermalKWh, dailySummary.dhw.electricalKWh);
  appendJsonFloat(client, dhwValid, dhwCop);
  appendText(client, ",\"outsideMinimum\":");
  appendJsonFloat(client, isfinite(dailySummary.outsideMinimum), dailySummary.outsideMinimum);
  appendText(client, ",\"outsideMaximum\":");
  appendJsonFloat(client, isfinite(dailySummary.outsideMaximum), dailySummary.outsideMaximum);
  appendText(client, ",\"outsideAverage\":");
  appendJsonFloat(client, isfinite(outsideAverage), outsideAverage);
  appendText(client, "}");
}

static void appendCyclesJson(struct webserver_t *client) {
  appendText(client, "[");
  for (uint8_t offset = 0; offset < cycleRecordCount; offset++) {
    if (offset > 0) appendText(client, ",");
    const CycleRecord &record = cycleRecords[(cycleRecordStart + offset) % 32];
    appendFmt(client, "{\"start\":%lu,\"stop\":%lu,\"durationSeconds\":%lu,\"state\":%u,\"outsideAverage\":",
      (unsigned long)record.startTimestamp, (unsigned long)record.stopTimestamp,
      (unsigned long)record.durationSeconds, record.operatingState);
    appendJsonFloat(client, isfinite(record.outsideAverage), record.outsideAverage);
    appendText(client, ",\"flowAverage\":");
    appendJsonFloat(client, isfinite(record.flowAverage), record.flowAverage);
    appendText(client, ",\"frequencyAverage\":");
    appendJsonFloat(client, isfinite(record.frequencyAverage), record.frequencyAverage);
    appendFmt(client, ",\"frequencyMaximum\":%.1f,\"thermalKWh\":%.4f,\"electricalKWh\":%.4f,\"cop\":",
      record.frequencyMaximum, record.thermalKWh, record.electricalKWh);
    appendJsonFloat(client, record.copValid, record.cop);
    appendText(client, "}");
  }
  appendText(client, "]");
}

static uint16_t orderedSampleIndex(uint16_t offset) {
  return (uint16_t)((sampleStart + offset) % HEISHAMON_HISTORY_MAX_SAMPLES);
}

struct SampleAggregate {
  HistorySample sample;
  float sums[17];
  uint16_t counts[17];
  float zone1RequestSum;
  uint16_t zone1RequestCount;
  int8_t heatingCurveShiftValue;
  uint16_t heatingCurveShiftCount;
  EnergyTotals energy;
  uint16_t count;
};

static void resetAggregate(SampleAggregate &aggregate) {
  memset(&aggregate, 0, sizeof(aggregate));
}

static void addAggregate(SampleAggregate &aggregate, const HistorySample &sample) {
  aggregate.sample.timestamp = sample.timestamp;
  aggregate.sample.uptimeSeconds = sample.uptimeSeconds;
  aggregate.sample.operatingMode = sample.operatingMode;
  aggregate.sample.valveState = sample.valveState;
  aggregate.sample.flags = sample.flags;
  aggregate.sample.operatingState = sample.operatingState;
  aggregate.sample.electricalSource = sample.electricalSource;
  aggregate.sample.validFields |= sample.validFields;
  if ((sample.validFields & HISTORY_FIELD_ZONE1_REQUEST) != 0) {
    if (aggregate.zone1RequestCount == 0) {
      aggregate.sample.zone1RequestSemantic = sample.zone1RequestSemantic;
      aggregate.sample.validFields |= HISTORY_FIELD_ZONE1_REQUEST;
    } else if (aggregate.sample.zone1RequestSemantic != sample.zone1RequestSemantic) {
      aggregate.sample.validFields &= ~HISTORY_FIELD_ZONE1_REQUEST;
    }
    if ((aggregate.sample.validFields & HISTORY_FIELD_ZONE1_REQUEST) != 0) {
      aggregate.zone1RequestSum += sample.zone1RequestValue10 / 10.0f;
    }
    aggregate.zone1RequestCount++;
  }
  if ((sample.validFields & HISTORY_FIELD_HEATING_CURVE_SHIFT) != 0) {
    if (aggregate.heatingCurveShiftCount == 0) {
      aggregate.heatingCurveShiftValue = sample.heatingCurveShift;
      aggregate.sample.heatingCurveShift = sample.heatingCurveShift;
      aggregate.sample.validFields |= HISTORY_FIELD_HEATING_CURVE_SHIFT;
    } else if (aggregate.heatingCurveShiftValue != sample.heatingCurveShift) {
      aggregate.sample.validFields &= ~HISTORY_FIELD_HEATING_CURVE_SHIFT;
    }
    aggregate.heatingCurveShiftCount++;
  }
  memcpy(aggregate.sample.externalValues, sample.externalValues,
    sizeof(aggregate.sample.externalValues));
  const uint32_t fields[] = {
    HISTORY_FIELD_OUTSIDE, HISTORY_FIELD_INLET, HISTORY_FIELD_OUTLET,
    HISTORY_FIELD_TARGET, HISTORY_FIELD_DHW, HISTORY_FIELD_FLOW,
    HISTORY_FIELD_COMPRESSOR_HZ, HISTORY_FIELD_PUMP_RPM,
    HISTORY_FIELD_THERMAL_POWER, HISTORY_FIELD_ELECTRICAL_POWER,
    HISTORY_FIELD_DHW_TARGET, HISTORY_FIELD_ROOM, HISTORY_FIELD_ROOM_TARGET,
    HISTORY_FIELD_HEAT_PRODUCTION, HISTORY_FIELD_HEAT_CONSUMPTION,
    HISTORY_FIELD_DHW_PRODUCTION, HISTORY_FIELD_DHW_CONSUMPTION
  };
  const float values[] = {
    sample.outsideTemp10 / 10.0f, sample.inletTemp10 / 10.0f,
    sample.outletTemp10 / 10.0f, sample.targetTemp10 / 10.0f,
    sample.dhwTemp10 / 10.0f, sample.flow100 / 100.0f,
    sample.compressorHz10 / 10.0f, (float)sample.pumpRpm,
    sample.thermalPower100 / 100.0f, (float)sample.electricalPowerW,
    sample.dhwTargetTemp10 / 10.0f, sample.roomTemp10 / 10.0f,
    sample.roomTarget10 / 10.0f, (float)sample.heatProductionW,
    (float)sample.heatConsumptionW, (float)sample.dhwProductionW,
    (float)sample.dhwConsumptionW
  };
  for (uint8_t i = 0; i < 17; i++) {
    if ((sample.validFields & fields[i]) != 0) {
      aggregate.sums[i] += values[i];
      aggregate.counts[i]++;
    }
  }
  aggregate.count++;
}

static void finishAggregate(SampleAggregate &aggregate) {
  const uint32_t fields[] = {
    HISTORY_FIELD_OUTSIDE, HISTORY_FIELD_INLET, HISTORY_FIELD_OUTLET,
    HISTORY_FIELD_TARGET, HISTORY_FIELD_DHW, HISTORY_FIELD_FLOW,
    HISTORY_FIELD_COMPRESSOR_HZ, HISTORY_FIELD_PUMP_RPM,
    HISTORY_FIELD_THERMAL_POWER, HISTORY_FIELD_ELECTRICAL_POWER,
    HISTORY_FIELD_DHW_TARGET, HISTORY_FIELD_ROOM, HISTORY_FIELD_ROOM_TARGET,
    HISTORY_FIELD_HEAT_PRODUCTION, HISTORY_FIELD_HEAT_CONSUMPTION,
    HISTORY_FIELD_DHW_PRODUCTION, HISTORY_FIELD_DHW_CONSUMPTION
  };
  for (uint8_t i = 0; i < 17; i++) {
    if (aggregate.counts[i] == 0) {
      aggregate.sample.validFields &= ~fields[i];
      continue;
    }
    float value = aggregate.sums[i] / aggregate.counts[i];
    switch (i) {
      case 0: aggregate.sample.outsideTemp10 = scaledSigned(value, 10.0f); break;
      case 1: aggregate.sample.inletTemp10 = scaledSigned(value, 10.0f); break;
      case 2: aggregate.sample.outletTemp10 = scaledSigned(value, 10.0f); break;
      case 3: aggregate.sample.targetTemp10 = scaledSigned(value, 10.0f); break;
      case 4: aggregate.sample.dhwTemp10 = scaledSigned(value, 10.0f); break;
      case 5: aggregate.sample.flow100 = scaledUnsigned(value, 100.0f); break;
      case 6: aggregate.sample.compressorHz10 = scaledUnsigned(value, 10.0f); break;
      case 7: aggregate.sample.pumpRpm = scaledUnsigned(value, 1.0f); break;
      case 8: aggregate.sample.thermalPower100 = scaledSigned(value, 100.0f); break;
      case 9: aggregate.sample.electricalPowerW = scaledUnsigned(value, 1.0f); break;
      case 10: aggregate.sample.dhwTargetTemp10 = scaledSigned(value, 10.0f); break;
      case 11: aggregate.sample.roomTemp10 = scaledSigned(value, 10.0f); break;
      case 12: aggregate.sample.roomTarget10 = scaledSigned(value, 10.0f); break;
      case 13: aggregate.sample.heatProductionW = scaledUnsigned(value, 1.0f); break;
      case 14: aggregate.sample.heatConsumptionW = scaledUnsigned(value, 1.0f); break;
      case 15: aggregate.sample.dhwProductionW = scaledUnsigned(value, 1.0f); break;
      case 16: aggregate.sample.dhwConsumptionW = scaledUnsigned(value, 1.0f); break;
    }
  }
  if (aggregate.zone1RequestCount > 0 &&
      (aggregate.sample.validFields & HISTORY_FIELD_ZONE1_REQUEST) != 0) {
    aggregate.sample.zone1RequestValue10 = scaledSigned(
      aggregate.zone1RequestSum / aggregate.zone1RequestCount, 10.0f);
  } else {
    aggregate.sample.validFields &= ~HISTORY_FIELD_ZONE1_REQUEST;
  }
  if (aggregate.heatingCurveShiftCount == 0 ||
      (aggregate.sample.validFields & HISTORY_FIELD_HEATING_CURVE_SHIFT) == 0) {
    aggregate.sample.validFields &= ~HISTORY_FIELD_HEATING_CURVE_SHIFT;
  }
}

static void appendSampleJson(struct webserver_t *client, const HistorySample &sample,
    bool useAggregatedCop = false, float aggregatedCop = 0.0f) {
  char outside[20], inlet[20], outlet[20], target[20], dhw[20];
  char dhwTarget[20], room[20], roomTarget[20], heatProduction[20];
  char heatConsumption[20], dhwProduction[20], dhwConsumption[20];
  char flow[20], hz[20], pump[20], power[20], electrical[20], cop[20], zone1Request[20], curveShift[20];
  const char *zone1Semantic = "unknown";
  snprintf(outside, sizeof(outside), (sample.validFields & HISTORY_FIELD_OUTSIDE) ? "%.1f" : "null", sample.outsideTemp10 / 10.0f);
  snprintf(inlet, sizeof(inlet), (sample.validFields & HISTORY_FIELD_INLET) ? "%.1f" : "null", sample.inletTemp10 / 10.0f);
  snprintf(outlet, sizeof(outlet), (sample.validFields & HISTORY_FIELD_OUTLET) ? "%.1f" : "null", sample.outletTemp10 / 10.0f);
  snprintf(target, sizeof(target), (sample.validFields & HISTORY_FIELD_TARGET) ? "%.1f" : "null", sample.targetTemp10 / 10.0f);
  snprintf(dhw, sizeof(dhw), (sample.validFields & HISTORY_FIELD_DHW) ? "%.1f" : "null", sample.dhwTemp10 / 10.0f);
  snprintf(dhwTarget, sizeof(dhwTarget), (sample.validFields & HISTORY_FIELD_DHW_TARGET) ? "%.1f" : "null", sample.dhwTargetTemp10 / 10.0f);
  snprintf(room, sizeof(room), (sample.validFields & HISTORY_FIELD_ROOM) ? "%.1f" : "null", sample.roomTemp10 / 10.0f);
  snprintf(roomTarget, sizeof(roomTarget), (sample.validFields & HISTORY_FIELD_ROOM_TARGET) ? "%.1f" : "null", sample.roomTarget10 / 10.0f);
  snprintf(flow, sizeof(flow), (sample.validFields & HISTORY_FIELD_FLOW) ? "%.2f" : "null", sample.flow100 / 100.0f);
  snprintf(hz, sizeof(hz), (sample.validFields & HISTORY_FIELD_COMPRESSOR_HZ) ? "%.1f" : "null", sample.compressorHz10 / 10.0f);
  snprintf(pump, sizeof(pump), (sample.validFields & HISTORY_FIELD_PUMP_RPM) ? "%u" : "null", sample.pumpRpm);
  snprintf(power, sizeof(power), (sample.validFields & HISTORY_FIELD_THERMAL_POWER) ? "%.2f" : "null", sample.thermalPower100 / 100.0f);
  snprintf(electrical, sizeof(electrical), (sample.validFields & HISTORY_FIELD_ELECTRICAL_POWER) ? "%.3f" : "null", sample.electricalPowerW / 1000.0f);
  snprintf(heatProduction, sizeof(heatProduction), (sample.validFields & HISTORY_FIELD_HEAT_PRODUCTION) ? "%.3f" : "null", sample.heatProductionW / 1000.0f);
  snprintf(heatConsumption, sizeof(heatConsumption), (sample.validFields & HISTORY_FIELD_HEAT_CONSUMPTION) ? "%.3f" : "null", sample.heatConsumptionW / 1000.0f);
  snprintf(dhwProduction, sizeof(dhwProduction), (sample.validFields & HISTORY_FIELD_DHW_PRODUCTION) ? "%.3f" : "null", sample.dhwProductionW / 1000.0f);
  snprintf(dhwConsumption, sizeof(dhwConsumption), (sample.validFields & HISTORY_FIELD_DHW_CONSUMPTION) ? "%.3f" : "null", sample.dhwConsumptionW / 1000.0f);
  bool zone1RequestValid = (sample.validFields & HISTORY_FIELD_ZONE1_REQUEST) != 0;
  snprintf(zone1Request, sizeof(zone1Request), zone1RequestValid ? "%.1f" : "null",
    sample.zone1RequestValue10 / 10.0f);
  snprintf(curveShift, sizeof(curveShift), (sample.validFields & HISTORY_FIELD_HEATING_CURVE_SHIFT) ? "%d" : "null", sample.heatingCurveShift);
  if (zone1RequestValid) {
    zone1Semantic = zone1HeatRequestSemanticName(
      (Zone1HeatRequestSemanticType)sample.zone1RequestSemantic);
  }
  bool copValid = useAggregatedCop ? isfinite(aggregatedCop) : instantaneousCop(sample, aggregatedCop);
  snprintf(cop, sizeof(cop), copValid ? "%.2f" : "null", aggregatedCop);
  appendFmt(client,
    "{\"t\":%lu,\"u\":%lu,\"outside\":%s,\"inlet\":%s,\"outlet\":%s,\"target\":%s,\"dhw\":%s,\"dhwTarget\":%s,\"room\":%s,\"roomTarget\":%s,\"flow\":%s,\"hz\":%s,\"pump\":%s,\"power\":%s,\"electrical\":%s,\"electricalSource\":%u,\"heatProduction\":%s,\"heatConsumption\":%s,\"dhwProduction\":%s,\"dhwConsumption\":%s,\"cop\":%s,\"zone1Request\":%s,\"zone1RequestSemantic\":\"%s\",\"heatingCurveShift\":%s,\"mode\":%u,\"valve\":%u,\"state\":%u,\"compressor\":%s,\"dhwActive\":%s,\"timeValid\":%s}",
    (unsigned long)sample.timestamp, (unsigned long)sample.uptimeSeconds,
    outside, inlet, outlet, target, dhw, dhwTarget, room, roomTarget, flow, hz, pump,
    power, electrical, sample.electricalSource, heatProduction, heatConsumption, dhwProduction, dhwConsumption, cop,
    zone1Request, zone1Semantic, curveShift,
    sample.operatingMode,
    sample.valveState, sample.operatingState,
    (sample.flags & SAMPLE_FLAG_COMPRESSOR) ? "true" : "false",
    (sample.flags & SAMPLE_FLAG_DHW) ? "true" : "false",
    (sample.flags & SAMPLE_FLAG_TIME_VALID) ? "true" : "false");
}

static void loadHistoryConfig() {
  if (!LittleFS.begin() || !LittleFS.exists("/history.json")) return;
  File file = LittleFS.open("/history.json", "r");
  if (!file) return;
  JsonDocument document;
  if (!deserializeJson(document, file)) {
    uint16_t configured = (uint16_t)(document["intervalSeconds"] | 0);
    if (configured >= 60 && configured <= 600 && configured % 60 == 0) {
      sampleIntervalSeconds = configured;
    }
    uint16_t retention = (uint16_t)(document["retentionDays"] | sdRetentionDays);
    if (retention == 0 || retention == 7 || retention == 14 || retention == 30 ||
        retention == 90) sdRetentionDays = retention;
    uint16_t source = (uint16_t)(document["electricalSource"] | 0);
    if (source <= 255) electricalSourceId = (uint8_t)source;
    float base = document["degreeDayBase"] | heatingDegreeDayBase;
    if (isfinite(base) && base >= 5.0f && base <= 30.0f) heatingDegreeDayBase = base;
  }
  file.close();
}

static bool saveHistoryConfig() {
  if (!LittleFS.begin()) return false;
  File file = LittleFS.open("/history.json", "w");
  if (!file) return false;
  JsonDocument document;
  document["version"] = 1;
  document["intervalSeconds"] = sampleIntervalSeconds;
  document["retentionDays"] = sdRetentionDays;
  document["electricalSource"] = electricalSourceId;
  document["degreeDayBase"] = heatingDegreeDayBase;
  bool ok = serializeJson(document, file) > 0;
  file.close();
  return ok;
}

#if HEISHAMON_SD_HISTORY_ENABLED
#if defined(ESP32)
// Keep the SD card on the second ESP32-S3 SPI controller.  The global SPI
// instance is FSPI and is used by the W5500 Ethernet driver.
static SPIClass sdSpi(HSPI);
#else
static SPIClass sdSpi(FSPI);
#endif

static void setSdError(const char *message) {
  sdState.active = false;
  snprintf(sdState.lastError, sizeof(sdState.lastError), "%s", message);
  char logLine[128];
  snprintf(logLine, sizeof(logLine), "[SD] %s", message);
  log_message(logLine);
}

static bool sdDatePath(char *path, size_t pathSize, const char *directory,
    const char *suffix) {
  time_t now = 0;
  if (!validClock(&now)) return false;
  struct tm local = {};
  if (localtime_r(&now, &local) == nullptr) return false;
  snprintf(path, pathSize, "%s/%04d-%02d-%02d.%s", directory,
    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, suffix);
  return true;
}

static void ensureSdDirectories() {
  if (!SD.exists("/history")) SD.mkdir("/history");
  if (!SD.exists("/events")) SD.mkdir("/events");
  if (!SD.exists("/daily")) SD.mkdir("/daily");
  if (!SD.exists("/cycles")) SD.mkdir("/cycles");
}

static bool parseSdDate(const char *name, int &year, int &month, int &day) {
  if (name == nullptr) return false;
  const char *base = strrchr(name, '/');
  base = base == nullptr ? name : base + 1;
  if (strlen(base) < 14 || base[4] != '-' || base[7] != '-' ||
      strncmp(base + 10, ".csv", 4) != 0) return false;
  char *end = nullptr;
  year = (int)strtol(base, &end, 10);
  if (end != base + 4) return false;
  month = (int)strtol(base + 5, &end, 10);
  if (end != base + 7) return false;
  day = (int)strtol(base + 8, &end, 10);
  return end == base + 10 && year >= 2020 && month >= 1 && month <= 12 &&
    day >= 1 && day <= 31;
}

static void pruneSdDirectory(const char *directory, time_t cutoff) {
  if (sdRetentionDays == 0) return;
  File root = SD.open(directory);
  if (!root) return;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    char name[64];
    snprintf(name, sizeof(name), "%s", entry.name());
    bool directoryEntry = entry.isDirectory();
    entry.close();
    if (directoryEntry) continue;
    int year = 0, month = 0, day = 0;
    if (!parseSdDate(name, year, month, day)) continue;
    struct tm fileDate = {};
    fileDate.tm_year = year - 1900;
    fileDate.tm_mon = month - 1;
    fileDate.tm_mday = day;
    fileDate.tm_hour = 12;
    time_t fileTime = mktime(&fileDate);
    if (fileTime != (time_t)-1 && fileTime < cutoff && SD.remove(name)) {
      char line[100];
      snprintf(line, sizeof(line), "[SD] Removed expired history file %s", name);
      log_message(line);
    }
  }
  root.close();
}

static void cleanupSdRetention() {
  if (sdRetentionDays == 0 || !sdState.active) return;
  time_t now = 0;
  if (!validClock(&now)) return;
  uint32_t day = (uint32_t)(now / 86400);
  if (day == sdLastRetentionDay) return;
  sdLastRetentionDay = day;
  pruneSdDirectory("/history", now - (time_t)sdRetentionDays * 86400);
  pruneSdDirectory("/events", now - (time_t)sdRetentionDays * 86400);
}

static void initializeSd() {
  if (HEISHAMON_SD_CS_PIN < 0) {
    setSdError("SD enabled but no CS pin configured");
    return;
  }
#if HEISHAMON_SD_SCK_PIN >= 0 && HEISHAMON_SD_MISO_PIN >= 0 && HEISHAMON_SD_MOSI_PIN >= 0
  sdSpi.begin(HEISHAMON_SD_SCK_PIN, HEISHAMON_SD_MISO_PIN,
    HEISHAMON_SD_MOSI_PIN, HEISHAMON_SD_CS_PIN);
#else
  sdSpi.begin();
#endif
  if (!SD.begin(HEISHAMON_SD_CS_PIN, sdSpi, HEISHAMON_SD_FREQUENCY)) {
    sdState.present = false;
    sdState.filesystemOk = false;
    setSdError("No card detected or filesystem unavailable");
    return;
  }
  sdState.present = true;
  sdState.filesystemOk = true;
  sdState.active = true;
  sdState.capacity = SD.cardSize();
  sdState.freeBytes = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
  ensureSdDirectories();
  cleanupSdRetention();
  log_message((char *)"[SD] Persistent history enabled");
}

static bool writeSdEvents() {
  if (!sdState.active || eventCount == 0) return true;
  char path[48];
  if (!sdDatePath(path, sizeof(path), "/events", "csv")) return false;
  bool exists = SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    setSdError("Could not open event file");
    return false;
  }
  if (!exists) file.println("timestamp,type,message,value");
  for (uint16_t offset = 0; offset < eventCount; offset++) {
    const HistoryEvent &event = events[(eventStart + offset) % HEISHAMON_HISTORY_MAX_EVENTS];
    if (event.sequence <= sdFlushedEventSequence) continue;
    file.printf("%lu,%s,\"%s\",%ld\n", (unsigned long)event.timestamp,
      eventTypeName(event.type), event.message, (long)event.value);
  }
  file.close();
  return true;
}

static bool writeSdSamples() {
  if (!sdState.active || sampleCount == 0) return true;
  char path[48];
  if (!sdDatePath(path, sizeof(path), "/history", "csv")) return false;
  bool exists = SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    setSdError("Could not open history file");
    return false;
  }
  if (!exists) file.println("timestamp,outside,inlet,outlet,target,dhw,dhw_target,room,room_target,flow,compressor_hz,pump_rpm,thermal_kw,electrical_kw,heat_production_kw,heat_consumption_kw,dhw_production_kw,dhw_consumption_kw,z1_request,z1_request_semantic,heating_curve_shift,mode,valve,state,defrost,estimated_cop");
  uint32_t oldestSequence = sampleSequence >= sampleCount ?
    sampleSequence - sampleCount + 1 : 1;
  if (sdFlushedSequence < oldestSequence - 1) sdFlushedSequence = oldestSequence - 1;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    const HistorySample &sample = samples[orderedSampleIndex(offset)];
    if (sample.sequence <= sdFlushedSequence) continue;
    float cop = 0;
    bool copValid = instantaneousCop(sample, cop);
    char outside[16], inlet[16], outlet[16], target[16], dhw[16], dhwTarget[16];
    char room[16], roomTarget[16], flow[16], hz[16], pump[16], thermal[16], electrical[16];
    char heatProduction[16], heatConsumption[16], dhwProduction[16], dhwConsumption[16], copText[16];
    char zone1Request[16], curveShift[16];
    const char *zone1Semantic = "";
    snprintf(outside, sizeof(outside), (sample.validFields & HISTORY_FIELD_OUTSIDE) ? "%.1f" : "", sample.outsideTemp10 / 10.0f);
    snprintf(inlet, sizeof(inlet), (sample.validFields & HISTORY_FIELD_INLET) ? "%.1f" : "", sample.inletTemp10 / 10.0f);
    snprintf(outlet, sizeof(outlet), (sample.validFields & HISTORY_FIELD_OUTLET) ? "%.1f" : "", sample.outletTemp10 / 10.0f);
    snprintf(target, sizeof(target), (sample.validFields & HISTORY_FIELD_TARGET) ? "%.1f" : "", sample.targetTemp10 / 10.0f);
    snprintf(dhw, sizeof(dhw), (sample.validFields & HISTORY_FIELD_DHW) ? "%.1f" : "", sample.dhwTemp10 / 10.0f);
    snprintf(dhwTarget, sizeof(dhwTarget), (sample.validFields & HISTORY_FIELD_DHW_TARGET) ? "%.1f" : "", sample.dhwTargetTemp10 / 10.0f);
    snprintf(room, sizeof(room), (sample.validFields & HISTORY_FIELD_ROOM) ? "%.1f" : "", sample.roomTemp10 / 10.0f);
    snprintf(roomTarget, sizeof(roomTarget), (sample.validFields & HISTORY_FIELD_ROOM_TARGET) ? "%.1f" : "", sample.roomTarget10 / 10.0f);
    snprintf(flow, sizeof(flow), (sample.validFields & HISTORY_FIELD_FLOW) ? "%.2f" : "", sample.flow100 / 100.0f);
    snprintf(hz, sizeof(hz), (sample.validFields & HISTORY_FIELD_COMPRESSOR_HZ) ? "%.1f" : "", sample.compressorHz10 / 10.0f);
    snprintf(pump, sizeof(pump), (sample.validFields & HISTORY_FIELD_PUMP_RPM) ? "%u" : "", sample.pumpRpm);
    snprintf(thermal, sizeof(thermal), (sample.validFields & HISTORY_FIELD_THERMAL_POWER) ? "%.2f" : "", sample.thermalPower100 / 100.0f);
    snprintf(electrical, sizeof(electrical), (sample.validFields & HISTORY_FIELD_ELECTRICAL_POWER) ? "%.3f" : "", sample.electricalPowerW / 1000.0f);
    snprintf(heatProduction, sizeof(heatProduction), (sample.validFields & HISTORY_FIELD_HEAT_PRODUCTION) ? "%.3f" : "", sample.heatProductionW / 1000.0f);
    snprintf(heatConsumption, sizeof(heatConsumption), (sample.validFields & HISTORY_FIELD_HEAT_CONSUMPTION) ? "%.3f" : "", sample.heatConsumptionW / 1000.0f);
    snprintf(dhwProduction, sizeof(dhwProduction), (sample.validFields & HISTORY_FIELD_DHW_PRODUCTION) ? "%.3f" : "", sample.dhwProductionW / 1000.0f);
    snprintf(dhwConsumption, sizeof(dhwConsumption), (sample.validFields & HISTORY_FIELD_DHW_CONSUMPTION) ? "%.3f" : "", sample.dhwConsumptionW / 1000.0f);
    snprintf(copText, sizeof(copText), copValid ? "%.2f" : "", cop);
    bool zone1RequestValid = (sample.validFields & HISTORY_FIELD_ZONE1_REQUEST) != 0;
    snprintf(zone1Request, sizeof(zone1Request), zone1RequestValid ? "%.1f" : "",
      sample.zone1RequestValue10 / 10.0f);
    if (zone1RequestValid) {
      zone1Semantic = zone1HeatRequestSemanticName(
        (Zone1HeatRequestSemanticType)sample.zone1RequestSemantic);
    }
    snprintf(curveShift, sizeof(curveShift), (sample.validFields & HISTORY_FIELD_HEATING_CURVE_SHIFT) ? "%d" : "",
      sample.heatingCurveShift);
    file.printf("%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%u,%u,%u,%u,%s\n",
      (unsigned long)sample.timestamp, outside, inlet, outlet, target, dhw,
      dhwTarget, room, roomTarget, flow, hz, pump, thermal, electrical,
      heatProduction, heatConsumption, dhwProduction, dhwConsumption,
      zone1Request, zone1Semantic, curveShift,
      sample.operatingMode, sample.valveState, sample.operatingState,
      (sample.flags & SAMPLE_FLAG_DEFROST) ? 1 : 0, copText);
  }
  file.close();
  sdState.lastWriteAt = millis();
  return true;
}

static void flushSd() {
  if (!sdState.active || (sampleCount == 0 && eventCount == 0)) return;
  if (writeSdSamples()) sdFlushedSequence = sampleSequence;
  if (writeSdEvents()) sdFlushedEventSequence = eventSequence;
}
#else
static void initializeSd() {
  sdState.supported = false;
  sdState.present = false;
  sdState.filesystemOk = false;
  sdState.active = false;
  snprintf(sdState.lastError, sizeof(sdState.lastError), "Disabled at build time");
}

static void flushSd() {}
static void cleanupSdRetention() {}
#endif

static void persistDailySummary(const DailySummary &summary) {
#if HEISHAMON_SD_HISTORY_ENABLED
  if (!summary.valid || !sdState.active) return;
  time_t value = (time_t)summary.dayStart;
  struct tm local = {};
  if (localtime_r(&value, &local) == nullptr) return;
  char path[48];
  snprintf(path, sizeof(path), "/daily/%04d-%02d-%02d.csv", local.tm_year + 1900,
    local.tm_mon + 1, local.tm_mday);
  bool exists = SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) return;
  if (!exists) file.println("day,heating_thermal_kwh,heating_electrical_kwh,heating_cop,dhw_thermal_kwh,dhw_electrical_kwh,dhw_cop,heating_degree_days,compressor_seconds,compressor_starts,outside_min,outside_max,outside_average");
  float heatingCop = 0, dhwCop = 0;
  bool heatingValid = totalsCop(summary.heating, heatingCop);
  bool dhwValid = totalsCop(summary.dhw, dhwCop);
  double outsideAverage = summary.outsideSamples == 0 ? NAN : summary.outsideSum / summary.outsideSamples;
  file.printf("%lu,%.4f,%.4f,%s,%.4f,%.4f,%s,%.4f,%lu,%lu,%s,%s,%s\n",
    (unsigned long)summary.dayStart, summary.heating.thermalKWh,
    summary.heating.electricalKWh, heatingValid ? String(heatingCop, 3).c_str() : "",
    summary.dhw.thermalKWh, summary.dhw.electricalKWh, dhwValid ? String(dhwCop, 3).c_str() : "",
    summary.heatingDegreeDays, (unsigned long)summary.compressorSeconds,
    (unsigned long)summary.compressorStarts,
    isfinite(summary.outsideMinimum) ? String(summary.outsideMinimum, 1).c_str() : "",
    isfinite(summary.outsideMaximum) ? String(summary.outsideMaximum, 1).c_str() : "",
    isfinite(outsideAverage) ? String(outsideAverage, 1).c_str() : "");
  file.close();
#else
  (void)summary;
#endif
}

static void persistCycleRecord(const CycleRecord &record) {
#if HEISHAMON_SD_HISTORY_ENABLED
  if (!sdState.active || record.stopTimestamp == 0) return;
  time_t value = (time_t)record.stopTimestamp;
  struct tm local = {};
  if (localtime_r(&value, &local) == nullptr) return;
  char path[48];
  snprintf(path, sizeof(path), "/cycles/%04d-%02d.csv", local.tm_year + 1900,
    local.tm_mon + 1);
  bool exists = SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) return;
  if (!exists) file.println("start,stop,duration_seconds,state,outside_average,flow_average,frequency_average,frequency_max,thermal_kwh,electrical_kwh,cop");
  char copText[16];
  snprintf(copText, sizeof(copText), record.copValid ? "%.3f" : "", record.cop);
  file.printf("%lu,%lu,%lu,%u,%s,%s,%s,%.2f,%.4f,%.4f,%s\n",
    (unsigned long)record.startTimestamp, (unsigned long)record.stopTimestamp,
    (unsigned long)record.durationSeconds, record.operatingState,
    isfinite(record.outsideAverage) ? String(record.outsideAverage, 2).c_str() : "",
    isfinite(record.flowAverage) ? String(record.flowAverage, 2).c_str() : "",
    isfinite(record.frequencyAverage) ? String(record.frequencyAverage, 2).c_str() : "",
    record.frequencyMaximum, record.thermalKWh, record.electricalKWh, copText);
  file.close();
#else
  (void)record;
#endif
}

static void setNullable(JsonObject object, const char *name, bool valid, float value) {
  if (valid && isfinite(value)) object[name] = value;
  else object[name] = nullptr;
}

static void addSensor(JsonArray sensors, const char *name, uint8_t topic,
    bool temperature, const char *unit) {
  JsonObject sensor = sensors.add<JsonObject>();
  sensor["name"] = name;
  sensor["source"] = "Panasonic TOP";
  sensor["topic"] = topic;
  sensor["unit"] = unit;
  float value = 0;
  bool valid = temperature ? readTemperature(topic, value) : readTopic(topic, value);
  sensor["valid"] = valid && dataFresh();
  if (lastHeatpumpDataAt == 0) sensor["ageSeconds"] = nullptr;
  else sensor["ageSeconds"] = (uint32_t)((millis() - lastHeatpumpDataAt) / 1000UL);
  if (valid) sensor["value"] = value;
  else sensor["value"] = nullptr;
}

static void appendDiagnosticsJson(JsonDocument &document) {
  bool fresh = dataFresh();
  uint32_t age = lastHeatpumpDataAt == 0 ? UINT32_MAX :
    (uint32_t)((millis() - lastHeatpumpDataAt) / 1000UL);

  JsonObject system = document["system"].to<JsonObject>();
  JsonObject panasonic = system["panasonic"].to<JsonObject>();
  panasonic["valid"] = frameHeaderValid();
  panasonic["fresh"] = fresh;
  if (age == UINT32_MAX) panasonic["ageSeconds"] = nullptr;
  else panasonic["ageSeconds"] = age;
  panasonic["status"] = fresh ? "OK" : frameHeaderValid() ? "STALE" : "NO DATA";
#if defined(ESP32)
  system["wifi"] = WiFi.status() == WL_CONNECTED;
  system["ethernet"] = ETH.hasIP();
  system["psramFound"] = psramFound();
  system["freePsram"] = psramFound() ? ESP.getFreePsram() : 0;
  system["resetReason"] = (uint32_t)esp_reset_reason();
#else
  system["wifi"] = WiFi.isConnected();
  system["ethernet"] = false;
  system["psramFound"] = false;
  system["freePsram"] = 0;
  system["resetReason"] = 0;
#endif
  system["mqtt"] = mqtt_client.connected();
  system["ntp"] = validClock();
  system["uptimeSeconds"] = (uint32_t)(millis() / 1000UL);
  system["freeHeap"] = ESP.getFreeHeap();
  system["firmware"] = heishamon_version;
  system["customVersion"] = CUSTOM_FEATURES_VERSION;
  system["heishamonBaseVersion"] = HEISHAMON_BASE_VERSION;

  JsonObject operation = document["operation"].to<JsonObject>();
  float value = 0;
  bool valid = readTopic(TOP_OPERATION_MODE, value);
  operation["mode"] = valid ? (int)lroundf(value) : -1;
  operation["compressorRunning"] = cycle.compressorRunning;
  operation["compressorFrequency"] = nullptr;
  if (readNonNegative(TOP_COMPRESSOR_HZ, value)) operation["compressorFrequency"] = value;
  operation["currentRuntimeSeconds"] = cycle.compressorRunning && cycle.compressorStartedAt != 0 ?
    (uint32_t)((millis() - cycle.compressorStartedAt) / 1000UL) : 0;
  operation["state"] = cycle.errorActive ? "Error" : cycle.defrostActive ? "Defrost" :
    cycle.dhwActive ? "DHW" : cycle.compressorRunning ? "Heating" :
    cycle.heatpumpOn ? "Circulation / standby" : "Off";
  if (readTopic(TOP_VALVE, value)) operation["valve"] = (int)lroundf(value);
  else operation["valve"] = nullptr;
  if (readNonNegative(TOP_FLOW, value)) operation["flow"] = value;
  else operation["flow"] = nullptr;
  if (readNonNegative(TOP_PUMP_SPEED, value)) operation["pumpSpeed"] = value;
  else operation["pumpSpeed"] = nullptr;
  if (readNonNegative(TOP_FAN1, value)) operation["fan1"] = value;
  else operation["fan1"] = nullptr;

  JsonObject hydraulics = document["hydraulics"].to<JsonObject>();
  float inlet = 0, outlet = 0, target = 0, flow = 0;
  bool inletValid = readTemperature(TOP_INLET, inlet);
  bool outletValid = readTemperature(TOP_OUTLET, outlet);
  bool targetValid = readTemperature(TOP_TARGET, target);
  bool flowValid = readNonNegative(TOP_FLOW, flow);
  setNullable(hydraulics, "inlet", inletValid, inlet);
  setNullable(hydraulics, "outlet", outletValid, outlet);
  setNullable(hydraulics, "target", targetValid, target);
  setNullable(hydraulics, "flow", flowValid, flow);
  bool deltaValid = inletValid && outletValid && fresh;
  float delta = outlet - inlet;
  setNullable(hydraulics, "deltaT", deltaValid, delta);
  bool waterLiquid = true;
  float liquid = 0;
  if (readTopic(TOP_LIQUID_TYPE, liquid)) waterLiquid = lroundf(liquid) == 0;
  bool powerValid = deltaValid && flowValid && flow > 0.2f && waterLiquid;
  float thermalPower = flow * delta * 0.0697f;
  setNullable(hydraulics, "calculatedThermalPowerKw", powerValid, thermalPower);
  hydraulics["calculatedThermalPowerLabel"] = "Calculated thermal power";
  bool targetErrorValid = outletValid && targetValid && fresh;
  setNullable(hydraulics, "targetError", targetErrorValid, outlet - target);
  float configuredDelta = 0;
  bool configuredDeltaValid = readTopic(23, configuredDelta);
  setNullable(hydraulics, "deltaTError", deltaValid && configuredDeltaValid,
    delta - configuredDelta);

  JsonObject control = document["control"].to<JsonObject>();
  JsonObject zone1Request = control["zone1HeatRequest"].to<JsonObject>();
  zone1HeatRequestSemanticToJson(zone1Request, actData,
    heishamonSettings.wpHeatMin, heishamonSettings.wpHeatMax);
  JsonObject curveShift = control["heatingCurveShift"].to<JsonObject>();
  heatingCurveShiftToJson(curveShift);
  if (readTemperature(TOP_ROOM_TEMP, value)) control["roomTemperature"] = value;
  else control["roomTemperature"] = nullptr;
  if (readTemperature(TOP_HEATING_OFF_OUTSIDE, value)) control["heatingOffOutside"] = value;
  else control["heatingOffOutside"] = nullptr;
  if (readTopic(TOP_HEATING_MODE, value)) control["heatingMode"] = (int)lroundf(value);
  else control["heatingMode"] = nullptr;

  JsonObject dhw = document["dhw"].to<JsonObject>();
  if (readTemperature(TOP_DHW_TEMP, value)) dhw["actual"] = value;
  else dhw["actual"] = nullptr;
  if (readTemperature(TOP_DHW_TARGET, value)) dhw["target"] = value;
  else dhw["target"] = nullptr;
  dhw["active"] = cycle.dhwActive;
  if (readTopic(TOP_FORCE_DHW, value)) dhw["forceState"] = (int)lroundf(value);
  else dhw["forceState"] = nullptr;
  if (readTopic(TOP_VALVE, value)) dhw["valveState"] = (int)lroundf(value);
  else dhw["valveState"] = nullptr;

  JsonObject counters = document["counters"].to<JsonObject>();
  if (readTopic(TOP_OPERATING_HOURS, value)) counters["panasonicOperationHours"] = value;
  else counters["panasonicOperationHours"] = nullptr;
  if (readTopic(TOP_OPERATION_COUNTER, value)) counters["panasonicOperationCounter"] = value;
  else counters["panasonicOperationCounter"] = nullptr;
  counters["compressorStartsSinceBoot"] = cycle.startsSinceBoot;
  counters["currentCycleSeconds"] = cycle.compressorRunning && cycle.compressorStartedAt != 0 ?
    (uint32_t)((millis() - cycle.compressorStartedAt) / 1000UL) : 0;
  counters["previousCycleSeconds"] = cycle.previousRunSeconds;
  counters["averageCycleSeconds"] = cycle.startsSinceBoot == 0 ? 0 :
    (uint32_t)(cycle.totalRunSeconds / cycle.startsSinceBoot);

  JsonObject internal = document["internal"].to<JsonObject>();
  const uint8_t internalTopics[] = {TOP_HEX_OUTLET, TOP_DISCHARGE, TOP_INSIDE_PIPE,
    TOP_DEFROST_TEMP, TOP_EVA_OUTLET, TOP_BYPASS_OUTLET, TOP_IPM,
    TOP_HIGH_PRESSURE, TOP_LOW_PRESSURE, TOP_FAN2};
  for (uint8_t topic : internalTopics) {
    float internalValue = 0;
    if (readTemperature(topic, internalValue) || readNonNegative(topic, internalValue)) internal[topics[topic]] = internalValue;
    else internal[topics[topic]] = nullptr;
  }

  JsonArray sensors = document["sensors"].to<JsonArray>();
  addSensor(sensors, "Outside temperature", TOP_OUTSIDE, true, "°C");
  addSensor(sensors, "Inlet temperature", TOP_INLET, true, "°C");
  addSensor(sensors, "Outlet temperature", TOP_OUTLET, true, "°C");
  addSensor(sensors, "Water flow", TOP_FLOW, false, "l/min");
  addSensor(sensors, "DHW temperature", TOP_DHW_TEMP, true, "°C");
  customFeaturesAppendExternalSensorDiagnostics(sensors);

  JsonObject history = document["history"].to<JsonObject>();
  history["sampleCount"] = sampleCount;
  history["capacity"] = HEISHAMON_HISTORY_MAX_SAMPLES;
  history["intervalSeconds"] = sampleIntervalSeconds;
  history["sampleMemoryBytes"] = diagnosticsHistorySampleMemoryBytes();
  history["eventMemoryBytes"] = diagnosticsHistoryEventMemoryBytes();

  JsonObject sd = document["sd"].to<JsonObject>();
  sd["support"] = sdState.supported;
  sd["present"] = sdState.present;
  sd["filesystem"] = sdState.filesystemOk;
  sd["active"] = sdState.active;
  sd["retentionDays"] = sdRetentionDays;
  sd["capacityBytes"] = sdState.capacity;
  sd["freeBytes"] = sdState.freeBytes;
  if (sdState.lastWriteAt == 0) sd["lastWriteSecondsAgo"] = nullptr;
  else sd["lastWriteSecondsAgo"] = (uint32_t)((millis() - sdState.lastWriteAt) / 1000UL);
  sd["lastError"] = sdState.lastError;
}

static const char diagnosticsPage[] PROGMEM = R"HTML(
<style>
.diagnostics-page{max-width:1500px;margin:0 auto}
.diagnostics-heading{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:16px}
.diagnostics-heading h1{font-size:20px;font-weight:500;color:var(--text-primary)}
.diagnostics-status{font-size:12px;color:var(--text-muted)}
.diagnostics-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:14px}
.diagnostics-card{min-width:0}
.diagnostics-rows{padding:8px 20px 12px}
.diagnostics-row{display:flex;justify-content:space-between;gap:14px;padding:8px 0;border-bottom:1px solid var(--border);font-size:12px;color:var(--text-secondary)}
.diagnostics-row:last-child{border-bottom:0}
.diagnostics-value{font-weight:600;color:var(--text-primary);text-align:right;overflow-wrap:anywhere}
.diagnostics-ok{color:var(--green)}
.diagnostics-bad{color:var(--red)}
.diagnostics-muted{color:var(--text-muted)}
.diagnostics-raw{margin-top:14px}
.diagnostics-raw summary{padding:14px 20px;cursor:pointer;font-size:13px;font-weight:500;color:var(--text-primary)}
.diagnostics-raw pre{margin:0 20px 20px;max-height:360px;overflow:auto;background:#0a0c0f;color:#6ee7b7;padding:12px;border-radius:var(--radius);font:11px 'JetBrains Mono',monospace}
@media(max-width:680px){.diagnostics-heading{align-items:flex-start;flex-direction:column}.diagnostics-grid{grid-template-columns:1fr}.diagnostics-rows{padding-left:14px;padding-right:14px}}
</style>
<main class='main-content diagnostics-page'>
<div class='diagnostics-heading'><h1>Diagnostics</h1><div id='status' class='diagnostics-status'>Loading…</div></div>
<div id='cards' class='diagnostics-grid'></div><details class='panel diagnostics-raw'><summary>Raw Panasonic values</summary><pre id='raw'>Loading…</pre></details>
</main>
<script>
function esc(v){return String(v==null?'N/A':v).replace(/[&<>"']/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c]})}
function val(v,unit){return v===null||v===undefined?'N/A':esc(v)+(unit||'')}
function row(k,v){return '<div class="diagnostics-row"><span>'+esc(k)+'</span><span class="diagnostics-value">'+v+'</span></div>'}
function card(title,rows){return '<section class="panel diagnostics-card"><div class="panel-header"><h3>'+title+'</h3></div><div class="diagnostics-rows">'+rows.join('')+'</div></section>'}
function render(d){var s=d.system||{},o=d.operation||{},h=d.hydraulics||{},c=d.control||{},z1=c.zone1HeatRequest||{},cs=c.heatingCurveShift||{},w=d.dhw||{},n=d.counters||{},sd=d.sd||{};
 document.getElementById('status').innerHTML='Panasonic: <b class="'+(s.panasonic&&s.panasonic.fresh?'diagnostics-ok':'diagnostics-bad')+'">'+esc(s.panasonic?s.panasonic.status:'NO DATA')+'</b> · Last frame: '+val(s.panasonic&&s.panasonic.ageSeconds,' s');
 document.getElementById('cards').innerHTML=card('SYSTEM STATUS',[row('WiFi',s.wifi?'Connected':'Offline'),row('Ethernet',s.ethernet?'Connected':'Offline'),row('MQTT',s.mqtt?'Connected':'Offline'),row('NTP',s.ntp?'Synchronized':'Unavailable'),row('Uptime',val(s.uptimeSeconds,' s')),row('Free heap',val(s.freeHeap,' B')),row('PSRAM',s.psramFound?val(s.freePsram,' B free'):'Not available'),row('Firmware',val(s.firmware))])+card('CURRENT OPERATION',[row('Interpreted state',val(o.state)),row('Operating mode',val(o.mode)),row('Compressor',o.compressorRunning?'Running':'Stopped'),row('Compressor frequency',val(o.compressorFrequency,' Hz')),row('Current runtime',val(o.currentRuntimeSeconds,' s')),row('Flow',val(o.flow,' l/min')),row('Pump speed',val(o.pumpSpeed,' rpm')),row('Fan 1',val(o.fan1,' rpm')),row('3-way valve',val(o.valve))])+card('HYDRAULICS',[row('Inlet',val(h.inlet,' °C')),row('Outlet',val(h.outlet,' °C')),row('Target',val(h.target,' °C')),row('Delta T',val(h.deltaT,' K')),row('Target error',val(h.targetError,' K')),row('Delta-T error',val(h.deltaTError,' K')),row('Calculated thermal power',val(h.calculatedThermalPowerKw,' kW'))])+card('CONTROL / DHW',[row('Zone 1 raw TOP27',val(z1.rawValue)),row(z1.label||'Zone 1 request',val(z1.value,z1.unit?' '+z1.unit:'')),row('Heating mode',val(z1.heatingModeLabel||c.heatingMode)),row('Zone 1 sensor setting',val(z1.sensorSettingLabel||z1.sensorSetting)),row('Heating-off outdoor temp',val(c.heatingOffOutside,' °C')),row('Room temperature',val(c.roomTemperature,' °C')),row('DHW actual',val(w.actual,' °C')),row('DHW target',val(w.target,' °C')),row('DHW active',w.active?'Yes':'No'),row('Force DHW',val(w.forceState)),row('Smart DHW','See Smart DHW page')])+card('COUNTERS',[row('Panasonic operation hours',val(n.panasonicOperationHours)),row('Panasonic operation counter',val(n.panasonicOperationCounter)),row('Compressor starts since boot',val(n.compressorStartsSinceBoot)),row('Current cycle',val(n.currentCycleSeconds,' s')),row('Previous cycle',val(n.previousCycleSeconds,' s')),row('Average cycle',val(n.averageCycleSeconds,' s'))])+card('PERSISTENT HISTORY',[row('RAM samples',val((d.history||{}).sampleCount)+' / '+val((d.history||{}).capacity)),row('Sample interval',val((d.history||{}).intervalSeconds,' s')),row('RAM memory',val((d.history||{}).sampleMemoryBytes,' B')),row('SD support',sd.support?'Enabled':'Disabled'),row('SD card',sd.present?'Present':'Not present'),row('History logging',sd.active?'Active':'RAM only'),row('Retention',sd.retentionDays===0?'Unlimited':val(sd.retentionDays,' days')),row('SD error',val(sd.lastError))]);
 document.getElementById('cards').insertAdjacentHTML('beforeend',card('HEATING CURVE SHIFT',[row('Requested shift',val(cs.shift,' K')),row('Implementation',val(cs.implementationLabel)),row('Base high',val(cs.baseTargetHigh,' °C')),row('Base low',val(cs.baseTargetLow,' °C')),row('Effective high',val(cs.effectiveTargetHigh,' °C')),row('Effective low',val(cs.effectiveTargetLow,' °C')),row('External mismatch',cs.externalMismatch?'Yes':'No')]));
 document.getElementById('raw').textContent=JSON.stringify(d,null,2);
}
function refresh(){fetch('/diagnosticsapi',{cache:'no-store'}).then(function(r){if(!r.ok)throw Error(r.status);return r.json()}).then(render).catch(function(e){document.getElementById('status').textContent='Diagnostics unavailable: '+e.message})}document.title='Diagnostics - HeishaMon';refresh();setInterval(refresh,5000);
</script>)HTML";

static const char historyPage[] PROGMEM = R"HTML(
<style>
.history-page{max-width:1500px;margin:0 auto}
.history-heading{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:16px}
.history-heading h1{font-size:20px;font-weight:500;color:var(--text-primary)}
.history-toolbar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:14px}
.history-toolbar label{display:flex;align-items:center;gap:6px;color:var(--text-secondary);font-size:12px}
.history-toolbar select{padding:7px 10px;border:1px solid var(--border);border-radius:var(--radius-sm);background:var(--bg-elevated);color:var(--text-primary);font:12px 'JetBrains Mono',monospace}
.history-status{color:var(--text-muted);font-size:12px;margin-left:auto}
.history-card{margin-bottom:14px}
.history-card-body{padding:14px 20px 16px}
.history-chart{display:block;width:100%;height:230px}
.history-legend{display:flex;gap:14px;align-items:center;flex-wrap:wrap;margin-top:10px;color:var(--text-secondary);font-size:11px}
.history-legend-item{display:inline-flex;align-items:center;gap:6px;white-space:nowrap}
.history-legend-line{display:inline-block;width:22px;height:3px;border-radius:2px}
.history-note{color:var(--text-muted);font-size:11px;line-height:1.5;margin-top:8px}
.history-events{max-height:250px;overflow:auto}
.history-event{padding:7px 0;border-bottom:1px solid var(--border);font-size:12px;color:var(--text-secondary)}
.history-event:last-child{border-bottom:0}
@media(max-width:680px){.history-heading{align-items:flex-start;flex-direction:column}.history-status{margin-left:0}.history-card-body{padding-left:14px;padding-right:14px}}
</style>
<main class='main-content history-page'>
<div class='history-heading'><h1>History</h1><span id='status' class='history-status'>Loading…</span></div>
<div class='history-toolbar'><label>Display range <select id='range' onchange='refresh()'><option value='30m'>Last 30 min</option><option value='1h' selected>Last 1 h</option><option value='3h'>Last 3 h</option><option value='all'>All RAM history</option></select></label><label>Storage interval <select id='interval' onchange='setIntervalValue()'><option value='1' selected>1 min</option><option value='2'>2 min</option><option value='3'>3 min</option><option value='4'>4 min</option><option value='5'>5 min</option><option value='6'>6 min</option><option value='7'>7 min</option><option value='8'>8 min</option><option value='9'>9 min</option><option value='10'>10 min</option></select></label><label>SD retention <select id='retention' onchange='setRetentionValue()'><option value='0'>Unlimited</option><option value='7'>7 days</option><option value='14'>14 days</option><option value='30' selected>30 days</option><option value='90'>90 days</option></select></label></div>
<div class='history-note' style='margin:-4px 0 14px'>Storage interval controls how often a new point is stored in RAM and on the SD card. Display range only changes the time range shown below.</div>
<section class='panel history-card'><div class='panel-header'><h3>Temperatures</h3></div><div class='history-card-body'><canvas id='temps' class='history-chart'></canvas><div id='tempsLegend' class='history-legend'></div><div class='history-note'>All values are in °C. The colored legend identifies every line.</div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>Zone 1 request</h3></div><div class='history-card-body'><canvas id='zone1Request' class='history-chart'></canvas><div id='zone1RequestLegend' class='history-legend'></div><div class='history-note'>Only the semantic series matching each sample is drawn. Heating curve shift (K), heating water target (°C) and room target (°C) are never merged into one line.</div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>Compressor / hydraulics</h3></div><div class='history-card-body'><canvas id='hydraulics' class='history-chart'></canvas><div id='hydraulicsLegend' class='history-legend'></div><div class='history-note'>The series use their native units: Hz, l/min and kW.</div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>Efficiency / COP</h3></div><div class='history-card-body'><div id='efficiencySummary' class='history-note' style='margin-top:0'></div><canvas id='efficiency' class='history-chart'></canvas><div id='efficiencyLegend' class='history-legend'></div><div class='history-note'>Estimated COP is calculated from thermal and electrical source values. Aggregated ranges use energy totals, not averaged COP samples.</div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>Weather / heating degree days</h3></div><div class='history-card-body'><div id='dailySummary' class='history-note' style='margin-top:0'>N/A</div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>Compressor cycles</h3></div><div class='history-card-body'><div id='cycles' class='history-events'>No completed cycles</div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>DHW</h3></div><div class='history-card-body'><canvas id='dhw' class='history-chart'></canvas><div id='dhwLegend' class='history-legend'></div></div></section>
<section class='panel history-card'><div class='panel-header'><h3>Events</h3></div><div class='history-card-body'><div id='events' class='history-events'></div></div></section>
<script>
var chartLines={outlet:{key:'outlet',label:'Outlet',color:'#e53935'},inlet:{key:'inlet',label:'Inlet',color:'#1e88e5'},target:{key:'target',label:'Target',color:'#f9a825'},outside:{key:'outside',label:'Outside',color:'#43a047'},hz:{key:'hz',label:'Compressor frequency',color:'#8e44ad'},flow:{key:'flow',label:'Water flow',color:'#00897b'},power:{key:'power',label:'Thermal power',color:'#ef6c00'},cop:{key:'cop',label:'Estimated COP',color:'#1565c0'},electrical:{key:'electrical',label:'Electrical power',color:'#c62828'},dhw:{key:'dhw',label:'DHW actual',color:'#d32f2f'},dhwTarget:{key:'target',label:'DHW target',color:'#f9a825'},zone1Shift:{key:'heatingCurveShift',label:'Heating curve shift (K)',color:'#6a1b9a'},zone1Water:{key:'zone1Request',semantic:'heatingWaterTarget',label:'Heating water target (°C)',color:'#00838f'},zone1Room:{key:'zone1Request',semantic:'roomTarget',label:'Room target (°C)',color:'#ad1457'}};
function lineLegend(id,lines){document.getElementById(id).innerHTML=lines.map(function(l){return '<span class="history-legend-item"><i class="history-legend-line" style="background:'+l.color+'"></i>'+l.label+'</span>'}).join('')}
function draw(id,points,lines){var c=document.getElementById(id),ctx=c.getContext('2d'),w=c.clientWidth||800,h=230,d=devicePixelRatio||1;c.width=w*d;c.height=h*d;ctx.scale(d,d);ctx.clearRect(0,0,w,h);if(!points.length)return;function matches(l,p){return !l.semantic||p.zone1RequestSemantic===l.semantic}var values=[];lines.forEach(function(l){points.forEach(function(p){var n=Number(p[l.key]);if(matches(l,p)&&p[l.key]!==null&&p[l.key]!==undefined&&Number.isFinite(n))values.push(n)})});if(!values.length)return;var min=Math.min.apply(null,values),max=Math.max.apply(null,values);if(min===max){min-=1;max+=1}function x(i){return 10+(w-25)*(i/(points.length-1||1))}function y(v){return h-18-(h-35)*(v-min)/(max-min)};ctx.font='11px Arial';ctx.fillStyle='#75818a';ctx.fillText(max.toFixed(1),4,14);ctx.fillText(min.toFixed(1),4,h-5);lines.forEach(function(l){ctx.strokeStyle=l.color;ctx.lineWidth=2;ctx.beginPath();var started=false;points.forEach(function(p,i){var n=Number(p[l.key]);if(!matches(l,p)||p[l.key]===null||p[l.key]===undefined||!Number.isFinite(n)){started=false;return}var px=x(i),py=y(n);if(!started){ctx.moveTo(px,py);started=true}else ctx.lineTo(px,py)});if(started)ctx.stroke()})}
function val(v,unit){return v===null||v===undefined?'N/A':Number(v).toFixed(2)+(unit||'')}
// Explicit units, grid/tick labels and a time axis keep the lightweight canvas
// charts readable without adding another browser-side chart library.
function draw(id,points,lines){var c=document.getElementById(id),ctx=c.getContext('2d'),w=c.clientWidth||800,h=230,d=devicePixelRatio||1;c.width=w*d;c.height=h*d;ctx.scale(d,d);ctx.clearRect(0,0,w,h);if(!points.length)return;function matches(l,p){return !l.semantic||p.zone1RequestSemantic===l.semantic}var values=[];lines.forEach(function(l){points.forEach(function(p){var n=Number(p[l.key]);if(matches(l,p)&&p[l.key]!==null&&p[l.key]!==undefined&&Number.isFinite(n))values.push(n)})});if(!values.length)return;var min=Math.min.apply(null,values),max=Math.max.apply(null,values);if(min===max){min-=1;max+=1}var left=48,right=12,top=18,bottom=34,plotW=w-left-right,plotH=h-top-bottom;function x(i){return left+plotW*(i/(points.length-1||1))}function y(v){return top+plotH-(plotH*(v-min)/(max-min))}function timeText(p){if(p&&p.timeValid&&Number(p.t)>1000000000)return new Date(Number(p.t)*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'});var last=Number(points[points.length-1].u||0),current=Number(p&&p.u||0);return '-'+Math.max(0,Math.round((last-current)/60))+'m'}var units=id==='temps'||id==='dhw'?'°C':id==='zone1Request'?'°C / K':id==='hydraulics'?'Hz / L/min / kW':'COP / kW';ctx.font='11px Arial';ctx.fillStyle='#75818a';ctx.strokeStyle='rgba(117,129,138,.22)';ctx.lineWidth=1;for(var tick=0;tick<=4;tick++){var number=max-(max-min)*tick/4,py=top+plotH*tick/4;ctx.beginPath();ctx.moveTo(left,py);ctx.lineTo(w-right,py);ctx.stroke();ctx.fillText(number.toFixed(1),4,py+4)}ctx.strokeStyle='#75818a';ctx.beginPath();ctx.moveTo(left,top);ctx.lineTo(left,top+plotH);ctx.lineTo(w-right,top+plotH);ctx.stroke();ctx.fillText(units,4,top-5);ctx.textAlign='center';[0,.25,.5,.75,1].forEach(function(f){var index=Math.round((points.length-1)*f);ctx.fillText(timeText(points[index]),left+plotW*f,h-12)});ctx.fillText('Time',left+plotW/2,h-1);ctx.textAlign='left';lines.forEach(function(l){ctx.strokeStyle=l.color;ctx.lineWidth=2;ctx.beginPath();var started=false;points.forEach(function(p,i){var n=Number(p[l.key]);if(!matches(l,p)||p[l.key]===null||p[l.key]===undefined||!Number.isFinite(n)){started=false;return}var px=x(i),py=y(n);if(!started){ctx.moveTo(px,py);started=true}else ctx.lineTo(px,py)});if(started)ctx.stroke()})}
function render(d){var p=d.samples||[],e2=d.efficiency||{},day=d.daily||{};var temps=[chartLines.outlet,chartLines.inlet,chartLines.target,chartLines.outside],zone1=[chartLines.zone1Shift,chartLines.zone1Water,chartLines.zone1Room],hydraulics=[chartLines.hz,chartLines.flow,chartLines.power],efficiency=[chartLines.cop,chartLines.power,chartLines.electrical],dhw=[chartLines.dhw,chartLines.dhwTarget];draw('temps',p,temps);lineLegend('tempsLegend',temps);draw('zone1Request',p,zone1);lineLegend('zone1RequestLegend',zone1);draw('hydraulics',p,hydraulics);lineLegend('hydraulicsLegend',hydraulics);draw('efficiency',p,efficiency);lineLegend('efficiencyLegend',efficiency);draw('dhw',p,dhw);lineLegend('dhwLegend',dhw);document.getElementById('efficiencySummary').innerHTML='Current estimated COP: <b>'+val(e2.currentEstimatedCop)+'</b> · Current cycle COP: <b>'+val(e2.currentCycleCop)+'</b> · Today heating COP: <b>'+val(e2.todayHeatingCop)+'</b> · Today DHW COP: <b>'+val(e2.todayDhwCop)+'</b> · Last 24 h COP: <b>'+val(e2.last24hCop)+'</b>';document.getElementById('dailySummary').innerHTML='Heating degree days: <b>'+val(day.heatingDegreeDays)+'</b> · Heating COP: <b>'+val(day.heatingCop)+'</b> · DHW COP: <b>'+val(day.dhwCop)+'</b> · Heating energy: <b>'+val(day.heatingThermalKWh)+' kWh</b> · DHW energy: <b>'+val(day.dhwThermalKWh)+' kWh</b> · Outside average: <b>'+val(day.outsideAverage)+' °C</b>';var cycles=d.cycles||[];document.getElementById('cycles').innerHTML=cycles.length?cycles.slice().reverse().map(function(x){return '<div class="history-event"><b>'+val(x.durationSeconds)+' s</b> · '+(x.state===3?'DHW':'Heating')+' · COP '+val(x.cop)+' · '+val(x.thermalKWh)+' kWh thermal</div>'}).join(''):'No completed cycles';var e=d.events||[];document.getElementById('events').innerHTML=e.length?e.slice().reverse().map(function(x){var stamp=x.timeValid?new Date((x.t||0)*1000).toLocaleString():'uptime '+(x.u||0)+' s';return '<div class="history-event"><b>'+stamp+'</b> '+x.type+': '+x.message+'</div>'}).join(''):'No events';document.getElementById('status').textContent=p.length+' samples · '+(d.intervalSeconds||0)+' s interval'}
function refresh(){fetch('/historyapi?range='+encodeURIComponent(document.getElementById('range').value),{cache:'no-store'}).then(function(r){if(!r.ok)throw Error(r.status);return r.json()}).then(render).catch(function(e){document.getElementById('status').textContent='History unavailable: '+e.message})}
function setIntervalValue(){fetch('/historycommand?interval='+document.getElementById('interval').value).then(refresh)}
function setRetentionValue(){fetch('/historycommand?retention='+document.getElementById('retention').value).then(refresh)}
function setElectricalSource(){fetch('/historycommand?electricalSource='+encodeURIComponent(document.getElementById('electricalSource').value)).then(refresh)}
function loadElectricalSources(){fetch('/externalsensorsapi',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){var label=document.createElement('label');label.textContent='Electrical source ';var select=document.createElement('select');select.id='electricalSource';select.onchange=setElectricalSource;var p=document.createElement('option');p.value='panasonic';p.textContent='Panasonic';select.appendChild(p);(d.sensors||[]).filter(function(s){return Number(s.role)===1}).forEach(function(s){var o=document.createElement('option');o.value='external:'+s.id;o.textContent=s.name+' (MQTT)';select.appendChild(o)});label.appendChild(select);document.querySelector('.toolbar').insertBefore(label,document.getElementById('status'));fetch('/history/status').then(function(r){return r.json()}).then(function(s){select.value=s.electricalSourceId?'external:'+s.electricalSourceId:'panasonic';});}).catch(function(){})}
loadElectricalSources();document.title='History - HeishaMon';fetch('/history/status').then(function(r){return r.json()}).then(function(s){document.getElementById('interval').value=String(Math.max(1,Math.min(10,Math.round((s.intervalSeconds||60)/60))));document.getElementById('retention').value=String(s.sdRetentionDays===undefined?30:s.sdRetentionDays)}).catch(function(){}).then(refresh);setInterval(refresh,10000);window.addEventListener('resize',refresh);
</script>)HTML";

static void sendJsonDocument(struct webserver_t *client, JsonDocument &document) {
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 19);
    appendText(client, "Diagnostics unavailable");
    return;
  }
  serializeJson(document, response, length + 1);
  webserver_send(client, 200, (char *)"application/json", length);
  webserver_send_content(client, response, length);
  free(response);
}

static void handleDiagnosticsPage(struct webserver_t *client) {
  webserver_send(client, 200, (char *)"text/html", 0);
  webserver_send_content_P(client, webHeader, strlen_P(webHeader));
  webserver_send_content_P(client, webCSS, strlen_P(webCSS));
  webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
  webserver_send_content_P(client, diagnosticsPage, strlen_P(diagnosticsPage));
  webserver_send_content_P(client, menuJS, strlen_P(menuJS));
  webserver_send_content_P(client, webFooter, strlen_P(webFooter));
}

static void handleHistoryPage(struct webserver_t *client) {
  webserver_send(client, 200, (char *)"text/html", 0);
  webserver_send_content_P(client, webHeader, strlen_P(webHeader));
  webserver_send_content_P(client, webCSS, strlen_P(webCSS));
  webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
  webserver_send_content_P(client, historyPage, strlen_P(historyPage));
  webserver_send_content_P(client, menuJS, strlen_P(menuJS));
  webserver_send_content_P(client, webFooter, strlen_P(webFooter));
}

static HistoryRequest *requestContext(struct webserver_t *client) {
  return (HistoryRequest *)client->userdata;
}

static void handleDiagnosticsApi(struct webserver_t *client) {
  JsonDocument document;
  appendDiagnosticsJson(document);
  sendJsonDocument(client, document);
}

static void handleHistoryStatus(struct webserver_t *client) {
  JsonDocument document;
  document["intervalSeconds"] = sampleIntervalSeconds;
  document["sampleCount"] = sampleCount;
  document["capacity"] = HEISHAMON_HISTORY_MAX_SAMPLES;
  document["sampleSizeBytes"] = sizeof(HistorySample);
  document["eventSizeBytes"] = sizeof(HistoryEvent);
  document["sampleMemoryBytes"] = diagnosticsHistorySampleMemoryBytes();
  document["eventMemoryBytes"] = diagnosticsHistoryEventMemoryBytes();
  document["sdSupport"] = sdState.supported;
  document["sdPresent"] = sdState.present;
  document["sdActive"] = sdState.active;
  document["sdRetentionDays"] = sdRetentionDays;
  document["electricalSource"] = electricalSourceId == 0 ? "panasonic" : "external";
  document["electricalSourceId"] = electricalSourceId;
  document["degreeDayBase"] = heatingDegreeDayBase;
  sendJsonDocument(client, document);
}

static void handleHistoryApi(struct webserver_t *client, uint32_t rangeSeconds,
    uint16_t maxPoints) {
  webserver_send(client, 200, (char *)"application/json", 0);
  appendFmt(client, "{\"intervalSeconds\":%u,\"sampleCount\":%u,\"efficiency\":",
    sampleIntervalSeconds, sampleCount);
  appendEfficiencySummary(client);
  appendText(client, ",\"daily\":");
  appendDailySummaryJson(client);
  appendText(client, ",\"cycles\":");
  appendCyclesJson(client);
  appendText(client, ",\"samples\":[");
  uint16_t matching = 0;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    if (sampleInRange(samples[orderedSampleIndex(offset)], rangeSeconds)) matching++;
  }
  uint16_t step = matching > maxPoints ? (uint16_t)((matching + maxPoints - 1) / maxPoints) : 1;
  uint16_t matchedIndex = 0;
  SampleAggregate aggregate;
  resetAggregate(aggregate);
  const HistorySample *previous = nullptr;
  bool first = true;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    const HistorySample &sample = samples[orderedSampleIndex(offset)];
    if (!sampleInRange(sample, rangeSeconds)) continue;
    addAggregate(aggregate, sample);
    if (previous != nullptr) {
      addEnergyPair(*previous, sample, 0, 0, UINT32_MAX, aggregate.energy);
    }
    previous = &sample;
    matchedIndex++;
    if (aggregate.count >= step || matchedIndex == matching) {
      finishAggregate(aggregate);
      float bucketCop = 0;
      bool bucketCopValid = totalsCop(aggregate.energy, bucketCop);
      if (!first) appendText(client, ",");
      first = false;
      appendSampleJson(client, aggregate.sample, step > 1,
        bucketCopValid ? bucketCop : NAN);
      resetAggregate(aggregate);
    }
  }
  appendText(client, "],\"events\":[");
  bool firstEvent = true;
  for (uint16_t offset = 0; offset < eventCount; offset++) {
    const HistoryEvent &event = events[(eventStart + offset) % HEISHAMON_HISTORY_MAX_EVENTS];
    if (rangeSeconds != 0 && currentTimestamp() >= event.timestamp &&
        currentTimestamp() - event.timestamp > rangeSeconds) continue;
    if (!firstEvent) appendText(client, ",");
    firstEvent = false;
    appendFmt(client, "{\"t\":%lu,\"u\":%lu,\"timeValid\":%s,\"type\":\"%s\",\"message\":\"%s\",\"value\":%ld}",
      (unsigned long)event.timestamp, (unsigned long)event.uptimeSeconds,
      event.timeValid ? "true" : "false", eventTypeName(event.type), event.message,
      (long)event.value);
  }
  appendText(client, "]}");
}

static void handleEventsApi(struct webserver_t *client) {
  webserver_send(client, 200, (char *)"application/json", 0);
  appendText(client, "[\n");
  for (uint16_t offset = 0; offset < eventCount; offset++) {
    if (offset > 0) appendText(client, ",\n");
    const HistoryEvent &event = events[(eventStart + offset) % HEISHAMON_HISTORY_MAX_EVENTS];
    appendFmt(client, "{\"t\":%lu,\"u\":%lu,\"timeValid\":%s,\"type\":\"%s\",\"message\":\"%s\",\"value\":%ld}",
      (unsigned long)event.timestamp, (unsigned long)event.uptimeSeconds,
      event.timeValid ? "true" : "false", eventTypeName(event.type), event.message,
      (long)event.value);
  }
  appendText(client, "]");
}

} // namespace

void diagnosticsHistoryBegin() {
  memset(samples, 0, sizeof(samples));
  memset(events, 0, sizeof(events));
  sampleStart = 0;
  sampleCount = 0;
  eventStart = 0;
  eventCount = 0;
  sampleSequence = 0;
  eventSequence = 0;
  sdFlushedSequence = 0;
  sdFlushedEventSequence = 0;
  lastSampleAt = 0;
  lastSdFlushAt = 0;
  cycle = CycleState();
  memset(cycleRecords, 0, sizeof(cycleRecords));
  cycleRecordStart = 0;
  cycleRecordCount = 0;
  dailySummary = DailySummary();
  memset(&previousDailySample, 0, sizeof(previousDailySample));
  previousDailySampleValid = false;
  communicationStateKnown = false;
  previousCommunicationState = false;
  mqttStateKnown = false;
  previousMqttState = false;
  loadHistoryConfig();
  initializeSd();
  char message[128];
  snprintf(message, sizeof(message),
    "[HISTORY] RAM history ready: %u samples, %u bytes samples, %u bytes events, interval %u s",
    HEISHAMON_HISTORY_MAX_SAMPLES, (unsigned)diagnosticsHistorySampleMemoryBytes(),
    (unsigned)diagnosticsHistoryEventMemoryBytes(), sampleIntervalSeconds);
  log_message(message);
}

void diagnosticsHistoryLoop() {
  bool communicationOk = dataFresh();
  if (!communicationStateKnown) {
    communicationStateKnown = true;
    previousCommunicationState = communicationOk;
  } else if (communicationOk != previousCommunicationState) {
    addEvent(communicationOk ? HISTORY_EVENT_COMMUNICATION : HISTORY_EVENT_COMMUNICATION,
      communicationOk ? "Panasonic communication restored" : "Panasonic communication lost");
    previousCommunicationState = communicationOk;
  }
  bool mqttOk = mqtt_client.connected();
  if (!mqttStateKnown) {
    mqttStateKnown = true;
    previousMqttState = mqttOk;
  } else if (mqttOk != previousMqttState) {
    addEvent(HISTORY_EVENT_MQTT, mqttOk ? "MQTT connected" : "MQTT disconnected");
    previousMqttState = mqttOk;
  }
  updateCycleState();
  unsigned long intervalMillis = (unsigned long)sampleIntervalSeconds * 1000UL;
  if (dataFresh() && (lastSampleAt == 0 || (unsigned long)(millis() - lastSampleAt) >= intervalMillis)) {
    HistorySample sample;
    if (makeSample(sample)) {
      sample.sequence = ++sampleSequence;
      storeSample(sample);
      updateDailySummary(samples[orderedSampleIndex(sampleCount - 1)]);
      lastSampleAt = millis();
    }
  }
  if (sdState.active && (lastSdFlushAt == 0 ||
      (unsigned long)(millis() - lastSdFlushAt) >= 60000UL)) {
    flushSd();
    cleanupSdRetention();
    lastSdFlushAt = millis();
  }
}

bool diagnosticsHistoryHandleUri(struct webserver_t *client, const char *uri) {
  if (strcmp(uri, "/diagnostics") == 0) client->route = ROUTE_DIAGNOSTICS;
  else if (strcmp(uri, "/diagnosticsapi") == 0 ||
      strcmp(uri, "/api/diagnostics") == 0) client->route = ROUTE_DIAGNOSTICS_API;
  else if (strcmp(uri, "/history") == 0) client->route = ROUTE_HISTORY;
  else if (strcmp(uri, "/history/status") == 0 ||
      strcmp(uri, "/api/history/status") == 0) client->route = ROUTE_HISTORY_STATUS;
  else if (strcmp(uri, "/historyapi") == 0 ||
      strcmp(uri, "/api/history") == 0) client->route = ROUTE_HISTORY_API;
  else if (strcmp(uri, "/events") == 0 ||
      strcmp(uri, "/api/events") == 0) client->route = ROUTE_EVENTS_API;
  else if (strcmp(uri, "/historycommand") == 0) client->route = ROUTE_HISTORY_COMMAND;
  else if (strcmp(uri, "/cycles") == 0 || strcmp(uri, "/api/cycles") == 0) client->route = ROUTE_CYCLES_API;
  else return false;

  if (client->route == ROUTE_HISTORY_API || client->route == ROUTE_HISTORY_COMMAND) {
    HistoryRequest *request = new HistoryRequest();
    if (request == nullptr) {
      log_message((char *)"[HISTORY] Out of memory while creating request");
      client->route = 0;
      return true;
    }
    request->rangeSeconds = 0;
    request->maxPoints = 600;
    request->response[0] = '\0';
    client->userdata = request;
  }
  return true;
}

bool diagnosticsHistoryHandleArgs(struct webserver_t *client,
    struct arguments_t *args) {
  if (client->route == ROUTE_HISTORY_API) {
    HistoryRequest *request = requestContext(client);
    if (request == nullptr) return true;
    if (strcmp((char *)args->name, "range") == 0) {
      char value[args->len + 1];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      uint32_t parsed = parseRange(value);
      if (parsed != UINT32_MAX) request->rangeSeconds = parsed;
    } else if (strcmp((char *)args->name, "maxPoints") == 0) {
      char value[args->len + 1];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      char *end = nullptr;
      long parsed = strtol(value, &end, 10);
      if (end != value && *end == '\0' && parsed >= 10 && parsed <= 1000) {
        request->maxPoints = (uint16_t)parsed;
      }
    }
    return true;
  }
  if (client->route == ROUTE_HISTORY_COMMAND) {
    HistoryRequest *request = requestContext(client);
    if (request == nullptr) return true;
    if (strcmp((char *)args->name, "interval") == 0) {
      char value[args->len + 1];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      uint16_t parsed = parseInterval(value);
      if (parsed == 0) snprintf(request->response, sizeof(request->response), "ERROR: storage interval must be 1..10 minutes");
      else {
        sampleIntervalSeconds = parsed;
        lastSampleAt = millis();
        snprintf(request->response, sizeof(request->response), "%s", saveHistoryConfig() ? "OK: history interval saved" : "ERROR: could not save history interval");
      }
    } else if (strcmp((char *)args->name, "retention") == 0) {
      char value[args->len + 1];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      uint16_t parsed = parseRetention(value);
      if (parsed == UINT16_MAX) snprintf(request->response, sizeof(request->response), "ERROR: retention must be 0, 7, 14, 30 or 90 days");
      else {
        sdRetentionDays = parsed;
        sdLastRetentionDay = 0;
        snprintf(request->response, sizeof(request->response), "%s", saveHistoryConfig() ? "OK: history retention saved" : "ERROR: could not save history retention");
      }
    } else if (strcmp((char *)args->name, "electricalSource") == 0) {
      char value[args->len + 1];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      if (strcmp(value, "panasonic") == 0) {
        electricalSourceId = 0;
      } else if (strncmp(value, "external:", 9) == 0) {
        char *end = nullptr;
        long parsed = strtol(value + 9, &end, 10);
        if (end == value + 9 || *end != '\0' || parsed < 1 || parsed > 255) {
          snprintf(request->response, sizeof(request->response), "ERROR: invalid electrical source");
          return true;
        }
        electricalSourceId = (uint8_t)parsed;
      } else {
        snprintf(request->response, sizeof(request->response), "ERROR: electrical source must be panasonic or external:<id>");
        return true;
      }
      snprintf(request->response, sizeof(request->response), "%s", saveHistoryConfig() ? "OK: electrical source saved" : "ERROR: could not save electrical source");
    } else if (strcmp((char *)args->name, "degreeDayBase") == 0) {
      char value[args->len + 1];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      char *end = nullptr;
      float parsed = strtof(value, &end);
      if (end == value || *end != '\0' || !isfinite(parsed) || parsed < 5.0f || parsed > 30.0f) {
        snprintf(request->response, sizeof(request->response), "ERROR: degree-day base must be between 5 and 30 C");
        return true;
      }
      heatingDegreeDayBase = parsed;
      snprintf(request->response, sizeof(request->response), "%s", saveHistoryConfig() ? "OK: degree-day base saved" : "ERROR: could not save degree-day base");
    }
    return true;
  }
  return false;
}

bool diagnosticsHistoryHandleWrite(struct webserver_t *client) {
  switch (client->route) {
    case ROUTE_DIAGNOSTICS:
      if (client->content == 0) handleDiagnosticsPage(client);
      return true;
    case ROUTE_DIAGNOSTICS_API:
      if (client->content == 0) handleDiagnosticsApi(client);
      return true;
    case ROUTE_HISTORY:
      if (client->content == 0) handleHistoryPage(client);
      return true;
    case ROUTE_HISTORY_STATUS:
      if (client->content == 0) handleHistoryStatus(client);
      return true;
    case ROUTE_HISTORY_API: {
      if (client->content == 0) {
        HistoryRequest *request = requestContext(client);
        handleHistoryApi(client, request == nullptr ? 0 : request->rangeSeconds,
          request == nullptr ? 600 : request->maxPoints);
        delete request;
        client->userdata = nullptr;
      }
      return true;
    }
    case ROUTE_EVENTS_API:
      if (client->content == 0) handleEventsApi(client);
      return true;
    case ROUTE_CYCLES_API:
      if (client->content == 0) {
        webserver_send(client, 200, (char *)"application/json", 0);
        appendCyclesJson(client);
      }
      return true;
    case ROUTE_HISTORY_COMMAND:
      if (client->content == 0) {
        HistoryRequest *request = requestContext(client);
        const char *response = request == nullptr || request->response[0] == '\0' ?
          "ERROR: no command" : request->response;
        webserver_send(client, 200, (char *)"text/plain", strlen(response));
        webserver_send_content(client, (char *)response, strlen(response));
        delete request;
        client->userdata = nullptr;
      }
      return true;
    default:
      return false;
  }
}

bool diagnosticsHistoryHandleClose(struct webserver_t *client) {
  if (client->route == ROUTE_HISTORY_API || client->route == ROUTE_HISTORY_COMMAND) {
    if (client->userdata != nullptr) {
      delete requestContext(client);
      client->userdata = nullptr;
    }
    return true;
  }
  return false;
}

void diagnosticsHistoryRecordEvent(HistoryEventType type, const char *message,
    int32_t value) {
  addEvent(type, message, value);
}

size_t diagnosticsHistorySampleMemoryBytes() {
  return sizeof(samples);
}

size_t diagnosticsHistoryEventMemoryBytes() {
  return sizeof(events);
}
