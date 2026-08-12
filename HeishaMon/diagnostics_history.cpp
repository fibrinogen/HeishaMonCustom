#include "diagnostics_history.h"

#include "decode.h"
#include "diagnostics_logic.h"
#include "history_config.h"
#include "version.h"
#include "webfunctions.h"
#include "zone1_heat_semantics.h"
#include "heating_curve_shift.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#if HEISHAMON_SD_HISTORY_ENABLED
#include <SD_MMC.h>
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
constexpr uint8_t ROUTE_PERSISTENT_EVENTS_API = 38;
constexpr uint8_t ROUTE_PERSISTENT_EVENTS_CSV = 39;

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
  uint32_t startTimestamp;
  uint32_t endTimestamp;
  uint16_t maxPoints;
  bool persistentStorage;
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

// History is a sizeable, non-time-critical cache. Keep it out of the small
// internal heap so web requests, Wi-Fi and Panasonic serial handling retain
// headroom. The ESP32-S3 target has PSRAM; no internal-RAM fallback is used.
static HistorySample *samples = nullptr;
static HistoryEvent *events = nullptr;
static bool historyBuffersReady = false;
#if defined(ESP32)
static portMUX_TYPE historyDataMux = portMUX_INITIALIZER_UNLOCKED;
#endif
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
static bool sdInitializationPending = false;
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
    case HISTORY_EVENT_SYSTEM: return "system";
    default: return "unknown";
  }
}

static void logHistoryEvent(HistoryEventType type, const char *message) {
  char line[128];
  snprintf(line, sizeof(line), "[HISTORY] %s: %s", eventTypeName(type), message);
  log_message(line);
}

static void addEvent(HistoryEventType type, const char *message, int32_t value = 0) {
  if (!historyBuffersReady) return;
  bool timestampValid = false;
  uint32_t timestamp = currentTimestamp(&timestampValid);
  uint32_t uptimeSeconds = (uint32_t)(millis() / 1000UL);
  char loggedMessage[sizeof(events[0].message)];
  snprintf(loggedMessage, sizeof(loggedMessage), "%s", message == nullptr ? "" : message);
#if defined(ESP32)
  portENTER_CRITICAL(&historyDataMux);
#endif
  uint16_t index;
  if (eventCount < HEISHAMON_HISTORY_MAX_EVENTS) {
    index = (uint16_t)((eventStart + eventCount) % HEISHAMON_HISTORY_MAX_EVENTS);
    eventCount++;
  } else {
    index = eventStart;
    eventStart = (uint16_t)((eventStart + 1) % HEISHAMON_HISTORY_MAX_EVENTS);
  }
  HistoryEvent &event = events[index];
  event.timestamp = timestamp;
  event.uptimeSeconds = uptimeSeconds;
  event.sequence = ++eventSequence;
  event.value = value;
  event.type = type;
  event.timeValid = timestampValid ? 1 : 0;
  snprintf(event.message, sizeof(event.message), "%s", loggedMessage);
#if defined(ESP32)
  portEXIT_CRITICAL(&historyDataMux);
#endif
  logHistoryEvent(type, loggedMessage);
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
  if (!historyBuffersReady) return;
#if defined(ESP32)
  portENTER_CRITICAL(&historyDataMux);
#endif
  uint16_t index = (uint16_t)((sampleStart + sampleCount) % HEISHAMON_HISTORY_MAX_SAMPLES);
  if (sampleCount < HEISHAMON_HISTORY_MAX_SAMPLES) sampleCount++;
  else sampleStart = (uint16_t)((sampleStart + 1) % HEISHAMON_HISTORY_MAX_SAMPLES);
  samples[index] = sample;
#if defined(ESP32)
  portEXIT_CRITICAL(&historyDataMux);
#endif
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

static bool parseHistoryTimestamp(const char *value, uint32_t &timestamp) {
  if (value == nullptr || *value == '\0') return false;
  char *end = nullptr;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed < 1704067200UL ||
      parsed > 4102444800UL) return false;
  timestamp = (uint32_t)parsed;
  return true;
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

#if HEISHAMON_SD_HISTORY_ENABLED && defined(ESP32)
enum SdWorkType : uint8_t {
  SD_WORK_FLUSH = 0,
  SD_WORK_DAILY,
  SD_WORK_CYCLE
};

enum SdWriterPhase : uint8_t {
  SD_PHASE_IDLE = 0,
  SD_PHASE_OPEN_SAMPLES,
  SD_PHASE_WRITE_SAMPLES,
  SD_PHASE_CLOSE_SAMPLES,
  SD_PHASE_OPEN_EVENTS,
  SD_PHASE_WRITE_EVENTS,
  SD_PHASE_CLOSE_EVENTS,
  SD_PHASE_RETENTION,
  SD_PHASE_DAILY,
  SD_PHASE_CYCLE
};

struct SdWorkItem {
  SdWorkType type;
  DailySummary daily;
  CycleRecord cycle;
};

static QueueHandle_t sdWorkQueue = nullptr;
static TaskHandle_t sdWriterTaskHandle = nullptr;
static volatile bool sdWriterBusy = false;
static volatile bool sdHistoryReaderBusy = false;
static volatile unsigned long sdWriterStartedAt = 0;
static volatile bool sdFlushQueued = false;
static volatile SdWriterPhase sdWriterPhase = SD_PHASE_IDLE;
static bool sdWriterStallReported = false;
static volatile bool sdWriterErrorPending = false;
static SemaphoreHandle_t sdFilesystemMutex = nullptr;
#endif

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
#if HEISHAMON_SD_HISTORY_ENABLED
static void writeDailySummaryToSd(const DailySummary &summary);
static void writeCycleRecordToSd(const CycleRecord &record);
#endif

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
  snprintf(path, pathSize, "/sd%s/%04d-%02d-%02d.%s", directory,
    local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, suffix);
  return true;
}

static bool ensureSdDirectory(const char *path) {
  struct stat info = {};
  if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
  if (mkdir(path, 0775) == 0) return true;
  char message[80];
  snprintf(message, sizeof(message), "mkdir %s failed: %d (%s)", path, errno,
    strerror(errno));
  setSdError(message);
  return false;
}

static bool ensureSdDirectories() {
  return ensureSdDirectory("/sd/history") && ensureSdDirectory("/sd/events") &&
    ensureSdDirectory("/sd/daily") && ensureSdDirectory("/sd/cycles");
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

#if defined(ESP32)
// The CSV files are deliberately read as a stream.  Long retention must not
// turn a History request into an in-RAM import of the complete SD archive.
constexpr uint8_t MAX_STORED_HISTORY_FILES = 96;
// These buffers are intentionally static rather than local to the HTTP
// handler. The Arduino loop task has a small stack and VFS::open() itself
// needs substantial stack space. Access is serialized by sdFilesystemMutex.
// Archive paths are only used by SD history reads. Keep this 6.9 KB list in
// PSRAM alongside the history cache, not in permanently reserved internal RAM.
static char (*storedArchivePaths)[72] = nullptr;
static char storedArchiveCsvLine[512];

typedef bool (*StoredHistoryVisitor)(const HistorySample &sample, void *context);

static bool parseCsvFloat(const char *text, float &value) {
  if (text == nullptr || *text == '\0') return false;
  char *end = nullptr;
  value = strtof(text, &end);
  return end != text && *end == '\0' && isfinite(value);
}

static bool parseCsvUnsigned(const char *text, uint32_t maximum, uint32_t &value) {
  if (text == nullptr || *text == '\0') return false;
  char *end = nullptr;
  unsigned long parsed = strtoul(text, &end, 10);
  if (end == text || *end != '\0' || parsed > maximum) return false;
  value = (uint32_t)parsed;
  return true;
}

static bool parseStoredHistorySample(char *line, HistorySample &sample) {
  if (line == nullptr || strncmp(line, "timestamp,", 10) == 0) return false;
  char *fields[26] = {};
  char *cursor = line;
  for (uint8_t index = 0; index < 26; index++) {
    fields[index] = cursor;
    char *separator = strchr(cursor, ',');
    if (separator == nullptr) {
      if (index != 25) return false;
      break;
    }
    *separator = '\0';
    cursor = separator + 1;
  }
  if (strchr(fields[25], ',') != nullptr) return false;

  memset(&sample, 0, sizeof(sample));
  uint32_t integer = 0;
  if (!parseCsvUnsigned(fields[0], UINT32_MAX, sample.timestamp) ||
      sample.timestamp < 1704067200UL) return false;
  sample.flags |= SAMPLE_FLAG_TIME_VALID;

  float value = 0;
#define SET_CSV_SIGNED(index, member, field, scale) \
  if (parseCsvFloat(fields[index], value)) { \
    sample.member = scaledSigned(value, scale); \
    sample.validFields |= field; \
  }
#define SET_CSV_UNSIGNED(index, member, field, scale) \
  if (parseCsvFloat(fields[index], value) && value >= 0.0f) { \
    sample.member = scaledUnsigned(value, scale); \
    sample.validFields |= field; \
  }
  SET_CSV_SIGNED(1, outsideTemp10, HISTORY_FIELD_OUTSIDE, 10.0f);
  SET_CSV_SIGNED(2, inletTemp10, HISTORY_FIELD_INLET, 10.0f);
  SET_CSV_SIGNED(3, outletTemp10, HISTORY_FIELD_OUTLET, 10.0f);
  SET_CSV_SIGNED(4, targetTemp10, HISTORY_FIELD_TARGET, 10.0f);
  SET_CSV_SIGNED(5, dhwTemp10, HISTORY_FIELD_DHW, 10.0f);
  SET_CSV_SIGNED(6, dhwTargetTemp10, HISTORY_FIELD_DHW_TARGET, 10.0f);
  SET_CSV_SIGNED(7, roomTemp10, HISTORY_FIELD_ROOM, 10.0f);
  SET_CSV_SIGNED(8, roomTarget10, HISTORY_FIELD_ROOM_TARGET, 10.0f);
  SET_CSV_UNSIGNED(9, flow100, HISTORY_FIELD_FLOW, 100.0f);
  SET_CSV_UNSIGNED(10, compressorHz10, HISTORY_FIELD_COMPRESSOR_HZ, 10.0f);
  SET_CSV_UNSIGNED(11, pumpRpm, HISTORY_FIELD_PUMP_RPM, 1.0f);
  SET_CSV_SIGNED(12, thermalPower100, HISTORY_FIELD_THERMAL_POWER, 100.0f);
  SET_CSV_UNSIGNED(13, electricalPowerW, HISTORY_FIELD_ELECTRICAL_POWER, 1000.0f);
  SET_CSV_UNSIGNED(14, heatProductionW, HISTORY_FIELD_HEAT_PRODUCTION, 1000.0f);
  SET_CSV_UNSIGNED(15, heatConsumptionW, HISTORY_FIELD_HEAT_CONSUMPTION, 1000.0f);
  SET_CSV_UNSIGNED(16, dhwProductionW, HISTORY_FIELD_DHW_PRODUCTION, 1000.0f);
  SET_CSV_UNSIGNED(17, dhwConsumptionW, HISTORY_FIELD_DHW_CONSUMPTION, 1000.0f);
#undef SET_CSV_SIGNED
#undef SET_CSV_UNSIGNED

  if (parseCsvFloat(fields[18], value)) {
    sample.zone1RequestValue10 = scaledSigned(value, 10.0f);
    if (strcmp(fields[19], "heatingWaterTarget") == 0) {
      sample.zone1RequestSemantic = ZONE1_HEATING_WATER_TARGET;
      sample.validFields |= HISTORY_FIELD_ZONE1_REQUEST;
    } else if (strcmp(fields[19], "roomTarget") == 0) {
      sample.zone1RequestSemantic = ZONE1_ROOM_TARGET;
      sample.validFields |= HISTORY_FIELD_ZONE1_REQUEST;
    }
  }
  if (parseCsvUnsigned(fields[20], 127, integer)) {
    sample.heatingCurveShift = (int8_t)integer;
    sample.validFields |= HISTORY_FIELD_HEATING_CURVE_SHIFT;
  } else if (parseCsvFloat(fields[20], value) && value >= -127.0f && value <= 127.0f &&
      value == lroundf(value)) {
    sample.heatingCurveShift = (int8_t)lroundf(value);
    sample.validFields |= HISTORY_FIELD_HEATING_CURVE_SHIFT;
  }
  if (parseCsvUnsigned(fields[21], 255, integer)) sample.operatingMode = (uint8_t)integer;
  if (parseCsvUnsigned(fields[22], 255, integer)) sample.valveState = (uint8_t)integer;
  if (parseCsvUnsigned(fields[23], HISTORY_STATE_TRANSITION, integer)) {
    sample.operatingState = (uint8_t)integer;
  }
  if (parseCsvUnsigned(fields[24], 1, integer) && integer != 0) {
    sample.flags |= SAMPLE_FLAG_DEFROST;
  }
  if ((sample.validFields & HISTORY_FIELD_COMPRESSOR_HZ) != 0 &&
      sample.compressorHz10 > 5) sample.flags |= SAMPLE_FLAG_COMPRESSOR;
  if (sample.operatingState == HISTORY_STATE_HEATING ||
      sample.operatingState == HISTORY_STATE_DHW ||
      sample.operatingState == HISTORY_STATE_DEFROST) sample.flags |= SAMPLE_FLAG_COMPRESSOR;
  if (sample.operatingState == HISTORY_STATE_DHW) sample.flags |= SAMPLE_FLAG_DHW;
  if (sample.operatingState == HISTORY_STATE_DEFROST) sample.flags |= SAMPLE_FLAG_DEFROST;
  return true;
}

static uint8_t collectStoredArchiveFiles(const char *directory,
    const char *posixDirectory, uint32_t lowerTimestamp) {
  if (storedArchivePaths == nullptr) return 0;
  File root = SD_MMC.open(directory);
  if (!root) return 0;
  time_t lowerTime = (time_t)lowerTimestamp;
  struct tm lowerDate = {};
  if (localtime_r(&lowerTime, &lowerDate) == nullptr) {
    root.close();
    return 0;
  }
  uint8_t count = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    char name[72] = {};
    snprintf(name, sizeof(name), "%s", entry.name());
    bool isDirectory = entry.isDirectory();
    entry.close();
    int year = 0, month = 0, day = 0;
    if (isDirectory || !parseSdDate(name, year, month, day)) continue;
    if (year < lowerDate.tm_year + 1900 ||
        (year == lowerDate.tm_year + 1900 && month < lowerDate.tm_mon + 1) ||
        (year == lowerDate.tm_year + 1900 && month == lowerDate.tm_mon + 1 &&
          day < lowerDate.tm_mday)) continue;
    if (count < MAX_STORED_HISTORY_FILES) {
      // Arduino File::name() intentionally returns only the final file name.
      // The CSV reader uses POSIX fopen(), which requires the SDMMC mount path.
      snprintf(storedArchivePaths[count], sizeof(storedArchivePaths[count]),
        "%s/%04d-%02d-%02d.csv", posixDirectory, year, month, day);
      count++;
    }
  }
  root.close();
  for (uint8_t first = 0; first < count; first++) {
    for (uint8_t second = first + 1; second < count; second++) {
      if (strcmp(storedArchivePaths[first], storedArchivePaths[second]) > 0) {
        char temporary[72];
        memcpy(temporary, storedArchivePaths[first], sizeof(temporary));
        memcpy(storedArchivePaths[first], storedArchivePaths[second], sizeof(temporary));
        memcpy(storedArchivePaths[second], temporary, sizeof(temporary));
      }
    }
  }
  return count;
}

static bool visitStoredHistory(uint32_t lowerTimestamp, uint32_t upperTimestamp,
    StoredHistoryVisitor visitor, void *context) {
  uint8_t fileCount = collectStoredArchiveFiles("/history", "/sd/history", lowerTimestamp);
  uint32_t processed = 0;
  for (uint8_t fileIndex = 0; fileIndex < fileCount; fileIndex++) {
    FILE *file = fopen(storedArchivePaths[fileIndex], "r");
    if (file == nullptr) continue;
    while (fgets(storedArchiveCsvLine, sizeof(storedArchiveCsvLine), file) != nullptr) {
      size_t length = strlen(storedArchiveCsvLine);
      while (length > 0 && (storedArchiveCsvLine[length - 1] == '\n' ||
          storedArchiveCsvLine[length - 1] == '\r')) {
        storedArchiveCsvLine[--length] = '\0';
      }
      HistorySample sample;
      if (!parseStoredHistorySample(storedArchiveCsvLine, sample) || sample.timestamp < lowerTimestamp ||
          sample.timestamp > upperTimestamp) continue;
      if (!visitor(sample, context)) {
        fclose(file);
        return false;
      }
      if ((++processed & 0x1fU) == 0) delay(0);
    }
    fclose(file);
  }
  return true;
}

static void appendStoredSampleJson(struct webserver_t *client,
    const HistorySample &sample, float aggregatedCop) {
  char outside[12], inlet[12], outlet[12], target[12], dhw[12], dhwTarget[12];
  char flow[12], hz[12], power[12], electrical[12], cop[12], request[12], shift[12];
  const char *semantic = "unknown";
#define FORMAT_FIELD(buffer, field, format, value) \
  snprintf(buffer, sizeof(buffer), (sample.validFields & field) ? format : "null", value)
  FORMAT_FIELD(outside, HISTORY_FIELD_OUTSIDE, "%.1f", sample.outsideTemp10 / 10.0f);
  FORMAT_FIELD(inlet, HISTORY_FIELD_INLET, "%.1f", sample.inletTemp10 / 10.0f);
  FORMAT_FIELD(outlet, HISTORY_FIELD_OUTLET, "%.1f", sample.outletTemp10 / 10.0f);
  FORMAT_FIELD(target, HISTORY_FIELD_TARGET, "%.1f", sample.targetTemp10 / 10.0f);
  FORMAT_FIELD(dhw, HISTORY_FIELD_DHW, "%.1f", sample.dhwTemp10 / 10.0f);
  FORMAT_FIELD(dhwTarget, HISTORY_FIELD_DHW_TARGET, "%.1f", sample.dhwTargetTemp10 / 10.0f);
  FORMAT_FIELD(flow, HISTORY_FIELD_FLOW, "%.2f", sample.flow100 / 100.0f);
  FORMAT_FIELD(hz, HISTORY_FIELD_COMPRESSOR_HZ, "%.1f", sample.compressorHz10 / 10.0f);
  FORMAT_FIELD(power, HISTORY_FIELD_THERMAL_POWER, "%.2f", sample.thermalPower100 / 100.0f);
  FORMAT_FIELD(electrical, HISTORY_FIELD_ELECTRICAL_POWER, "%.3f", sample.electricalPowerW / 1000.0f);
  FORMAT_FIELD(request, HISTORY_FIELD_ZONE1_REQUEST, "%.1f", sample.zone1RequestValue10 / 10.0f);
  FORMAT_FIELD(shift, HISTORY_FIELD_HEATING_CURVE_SHIFT, "%d", sample.heatingCurveShift);
#undef FORMAT_FIELD
  if ((sample.validFields & HISTORY_FIELD_ZONE1_REQUEST) != 0) {
    semantic = zone1HeatRequestSemanticName(
      (Zone1HeatRequestSemanticType)sample.zone1RequestSemantic);
  }
  snprintf(cop, sizeof(cop), isfinite(aggregatedCop) ? "%.2f" : "null", aggregatedCop);
  appendFmt(client,
    "{\"t\":%lu,\"outside\":%s,\"inlet\":%s,\"outlet\":%s,\"target\":%s,\"dhw\":%s,\"dhwTarget\":%s,\"flow\":%s,\"hz\":%s,\"power\":%s,\"electrical\":%s,\"cop\":%s,\"zone1Request\":%s,\"zone1RequestSemantic\":\"%s\",\"heatingCurveShift\":%s,\"timeValid\":true}",
    (unsigned long)sample.timestamp, outside, inlet, outlet, target, dhw,
    dhwTarget, flow, hz, power, electrical, cop, request, semantic, shift);
}

// A History response is intentionally built in one SD pass. The old approach
// counted all matching CSV rows and then scanned them a second time to choose
// every nth sample. Time buckets preserve chronological display while keeping
// the shared SD lock for roughly half as long.
constexpr uint16_t MAX_STORED_HISTORY_DISPLAY_POINTS = 72;
struct StoredHistoryOutput {
  uint32_t lowerTimestamp;
  uint32_t upperTimestamp;
  uint16_t maxPoints;
  uint32_t sampleCount;
  EnergyTotals heating;
  EnergyTotals dhw;
  HistorySample previous = {};
  bool previousValid = false;
  uint16_t activeBucket = UINT16_MAX;
  SampleAggregate aggregate = {};
  HistorySample bucketPrevious = {};
  bool bucketPreviousValid = false;
  HistorySample *samples;
  float *cops;
  uint16_t emitted;
};

static void finishStoredHistoryBucket(StoredHistoryOutput &output) {
  if (output.aggregate.count == 0 || output.emitted >= output.maxPoints) return;
  finishAggregate(output.aggregate);
  float cop = NAN;
  totalsCop(output.aggregate.energy, cop);
  output.samples[output.emitted] = output.aggregate.sample;
  output.cops[output.emitted] = cop;
  output.emitted++;
  resetAggregate(output.aggregate);
  output.bucketPreviousValid = false;
}

static bool aggregateStoredHistorySample(const HistorySample &sample, void *context) {
  StoredHistoryOutput &output = *(StoredHistoryOutput *)context;
  if (output.previousValid) {
    addEnergyPair(output.previous, sample, 1, output.lowerTimestamp,
      output.upperTimestamp, output.heating);
    addEnergyPair(output.previous, sample, 2, output.lowerTimestamp,
      output.upperTimestamp, output.dhw);
  }
  output.previous = sample;
  output.previousValid = true;
  output.sampleCount++;

  uint16_t bucket = (uint16_t)(((uint64_t)(sample.timestamp - output.lowerTimestamp) *
    output.maxPoints) / (output.upperTimestamp - output.lowerTimestamp));
  if (bucket >= output.maxPoints) bucket = output.maxPoints - 1;
  if (output.activeBucket != UINT16_MAX && bucket != output.activeBucket) {
    finishStoredHistoryBucket(output);
  }
  if (output.activeBucket != bucket) output.activeBucket = bucket;
  addAggregate(output.aggregate, sample);
  if (output.bucketPreviousValid) {
    addEnergyPair(output.bucketPrevious, sample, 0, output.lowerTimestamp,
      output.upperTimestamp,
      output.aggregate.energy);
  }
  output.bucketPrevious = sample;
  output.bucketPreviousValid = true;
  return true;
}

static void handleStoredHistoryApi(struct webserver_t *client, uint32_t lowerTimestamp,
    uint32_t upperTimestamp, uint16_t maxPoints) {
  constexpr uint32_t MAX_STORED_HISTORY_RANGE_SECONDS = 90UL * 86400UL;
  if (!sdState.active) {
    webserver_send(client, 503, (char *)"application/json", 0);
    appendFmt(client, "{\"error\":\"Persistent SD history is inactive: %s\"}",
      sdState.lastError);
    return;
  }
  if (!validClock()) {
    webserver_send(client, 503, (char *)"application/json", 0);
    appendText(client, "{\"error\":\"Controller clock is not synchronized\"}");
    return;
  }
  if (sdFilesystemMutex == nullptr) {
    webserver_send(client, 503, (char *)"application/json", 0);
    appendText(client, "{\"error\":\"SD history reader is not initialized\"}");
    return;
  }
  if (lowerTimestamp == 0 || upperTimestamp <= lowerTimestamp ||
      upperTimestamp - lowerTimestamp > MAX_STORED_HISTORY_RANGE_SECONDS) {
    webserver_send(client, 400, (char *)"application/json", 0);
    appendText(client, "{\"error\":\"Select a valid SD period of up to 90 days\"}");
    return;
  }
  if (sdHistoryReaderBusy || xSemaphoreTake(sdFilesystemMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    webserver_send(client, 429, (char *)"application/json", 0);
    appendText(client, "{\"error\":\"SD history is busy; retry shortly\"}");
    return;
  }
  sdHistoryReaderBusy = true;
  unsigned long readStartedAt = millis();
  uint32_t rangeSeconds = upperTimestamp - lowerTimestamp;
  // The response buffer is temporary but too large for the Arduino loop-task
  // stack. Keep it in PSRAM so queued HTTP chunks still have sufficient
  // internal heap. The ESP32-S3 target has PSRAM; fail safely if unavailable.
  HistorySample *displaySamples = psramFound() ? (HistorySample *)ps_malloc(
    sizeof(HistorySample) * MAX_STORED_HISTORY_DISPLAY_POINTS) : nullptr;
  float *displayCops = psramFound() ? (float *)ps_malloc(
    sizeof(float) * MAX_STORED_HISTORY_DISPLAY_POINTS) : nullptr;
  if (displaySamples == nullptr || displayCops == nullptr) {
    free(displaySamples);
    free(displayCops);
    sdHistoryReaderBusy = false;
    xSemaphoreGive(sdFilesystemMutex);
    webserver_send(client, 503, (char *)"application/json", 0);
    appendText(client, "{\"error\":\"Insufficient PSRAM for SD history response\"}");
    return;
  }
  StoredHistoryOutput output = {};
  memset(&output, 0, sizeof(output));
  output.lowerTimestamp = lowerTimestamp;
  output.upperTimestamp = upperTimestamp;
  output.maxPoints = min<uint16_t>(maxPoints, MAX_STORED_HISTORY_DISPLAY_POINTS);
  output.samples = displaySamples;
  output.cops = displayCops;
  bool read = visitStoredHistory(lowerTimestamp, upperTimestamp,
    aggregateStoredHistorySample, &output);
  finishStoredHistoryBucket(output);
  if (!read) {
    free(displayCops);
    free(displaySamples);
    sdHistoryReaderBusy = false;
    xSemaphoreGive(sdFilesystemMutex);
    webserver_send(client, 500, (char *)"application/json", 0);
    appendText(client, "{\"error\":\"Could not read persistent history\"}");
    return;
  }
  float heatingCop = NAN, dhwCop = NAN;
  totalsCop(output.heating, heatingCop);
  totalsCop(output.dhw, dhwCop);
  webserver_send(client, 200, (char *)"application/json", 0);
  appendFmt(client,
    "{\"source\":\"sd\",\"intervalSeconds\":%u,\"storedSampleCount\":%lu,\"rangeSeconds\":%lu,\"efficiency\":{\"heatingThermalKWh\":%.4f,\"heatingElectricalKWh\":%.4f,\"heatingCop\":",
    sampleIntervalSeconds, (unsigned long)output.sampleCount, (unsigned long)rangeSeconds,
    output.heating.thermalKWh, output.heating.electricalKWh);
  appendJsonFloat(client, isfinite(heatingCop), heatingCop);
  appendFmt(client, ",\"dhwThermalKWh\":%.4f,\"dhwElectricalKWh\":%.4f,\"dhwCop\":",
    output.dhw.thermalKWh, output.dhw.electricalKWh);
  appendJsonFloat(client, isfinite(dhwCop), dhwCop);
  appendText(client, "},\"samples\":[");
  for (uint16_t index = 0; index < output.emitted; index++) {
    if (index > 0) appendText(client, ",");
    appendStoredSampleJson(client, output.samples[index], output.cops[index]);
  }
  appendText(client, "]}");
  free(displayCops);
  free(displaySamples);
  sdHistoryReaderBusy = false;
  xSemaphoreGive(sdFilesystemMutex);
  char timingMessage[128];
  snprintf(timingMessage, sizeof(timingMessage),
    "[HISTORY] SD read: %lu samples, %u display points in %lu ms",
    (unsigned long)output.sampleCount, output.emitted,
    (unsigned long)(millis() - readStartedAt));
  log_message(timingMessage);
}

struct PersistentEventLine {
  uint32_t timestamp;
  int32_t value;
  char type[32];
  char message[96];
};

typedef bool (*PersistentEventVisitor)(const PersistentEventLine &event, void *context);

static void sanitizePersistentEventText(char *text) {
  if (text == nullptr) return;
  for (; *text; text++) {
    unsigned char value = (unsigned char)*text;
    if (value < 0x20 || *text == '"' || *text == '\\') *text = ' ';
  }
}

static bool parsePersistentEventLine(char *line, PersistentEventLine &event) {
  if (line == nullptr || strncmp(line, "timestamp,", 10) == 0) return false;
  char *first = strchr(line, ',');
  if (first == nullptr) return false;
  *first = '\0';
  uint32_t timestamp = 0;
  if (!parseCsvUnsigned(line, UINT32_MAX, timestamp) || timestamp < 1704067200UL) return false;
  char *second = strchr(first + 1, ',');
  if (second == nullptr) return false;
  *second = '\0';
  char *message = second + 1;
  char *valueText = nullptr;
  if (*message == '"') {
    message++;
    char *closingQuote = strrchr(message, '"');
    if (closingQuote == nullptr || closingQuote[1] != ',') return false;
    *closingQuote = '\0';
    valueText = closingQuote + 2;
  } else {
    valueText = strrchr(message, ',');
    if (valueText == nullptr) return false;
    *valueText++ = '\0';
  }
  char *end = nullptr;
  long value = strtol(valueText, &end, 10);
  if (end == valueText || *end != '\0' || value < INT32_MIN || value > INT32_MAX) return false;
  memset(&event, 0, sizeof(event));
  event.timestamp = timestamp;
  event.value = (int32_t)value;
  strlcpy(event.type, first + 1, sizeof(event.type));
  strlcpy(event.message, message, sizeof(event.message));
  sanitizePersistentEventText(event.type);
  sanitizePersistentEventText(event.message);
  return true;
}

static bool visitPersistentEventLog(uint32_t lowerTimestamp, uint32_t upperTimestamp,
    PersistentEventVisitor visitor, void *context) {
  uint8_t fileCount = collectStoredArchiveFiles("/events", "/sd/events", lowerTimestamp);
  uint32_t processed = 0;
  for (uint8_t fileIndex = 0; fileIndex < fileCount; fileIndex++) {
    FILE *file = fopen(storedArchivePaths[fileIndex], "r");
    if (file == nullptr) continue;
    while (fgets(storedArchiveCsvLine, sizeof(storedArchiveCsvLine), file) != nullptr) {
      size_t length = strlen(storedArchiveCsvLine);
      while (length > 0 && (storedArchiveCsvLine[length - 1] == '\n' ||
          storedArchiveCsvLine[length - 1] == '\r')) storedArchiveCsvLine[--length] = '\0';
      PersistentEventLine event;
      if (!parsePersistentEventLine(storedArchiveCsvLine, event) ||
          event.timestamp < lowerTimestamp || event.timestamp > upperTimestamp) continue;
      if (!visitor(event, context)) {
        fclose(file);
        return false;
      }
      if ((++processed & 0x1fU) == 0) delay(0);
    }
    fclose(file);
  }
  return true;
}

struct PersistentEventCount {
  uint32_t count = 0;
};

static bool countPersistentEvent(const PersistentEventLine &, void *context) {
  ((PersistentEventCount *)context)->count++;
  return true;
}

struct PersistentEventOutput {
  struct webserver_t *client;
  uint32_t skip;
  uint32_t index = 0;
  bool csv;
  bool first = true;
};

static bool appendPersistentEvent(const PersistentEventLine &event, void *context) {
  PersistentEventOutput &output = *(PersistentEventOutput *)context;
  if (output.index++ < output.skip) return true;
  if (output.csv) {
    appendFmt(output.client, "%lu,%s,\"%s\",%ld\n", (unsigned long)event.timestamp,
      event.type, event.message, (long)event.value);
    return true;
  }
  if (!output.first) appendText(output.client, ",");
  output.first = false;
  appendFmt(output.client, "{\"t\":%lu,\"type\":\"%s\",\"message\":\"%s\",\"value\":%ld}",
    (unsigned long)event.timestamp, event.type, event.message, (long)event.value);
  return true;
}

static void handlePersistentEventLog(struct webserver_t *client, uint32_t lowerTimestamp,
    uint32_t upperTimestamp, bool csv) {
  constexpr uint32_t MAX_EVENT_LOG_RANGE_SECONDS = 90UL * 86400UL;
  constexpr uint32_t MAX_RETURNED_EVENTS = 100;
  if (!sdState.active || !validClock() || lowerTimestamp == 0 ||
      upperTimestamp <= lowerTimestamp ||
      upperTimestamp - lowerTimestamp > MAX_EVENT_LOG_RANGE_SECONDS ||
      sdFilesystemMutex == nullptr) {
    webserver_send(client, 400, (char *)(csv ? "text/csv" : "application/json"), 0);
    appendText(client, csv ? "timestamp,type,message,value\n" :
      "{\"error\":\"Select a valid event-log period of up to 90 days\"}");
    return;
  }
  if (xSemaphoreTake(sdFilesystemMutex, pdMS_TO_TICKS(250)) != pdTRUE) {
    webserver_send(client, 503, (char *)(csv ? "text/csv" : "application/json"), 0);
    appendText(client, csv ? "timestamp,type,message,value\n" :
      "{\"error\":\"SD card is busy writing history\"}");
    return;
  }
  sdHistoryReaderBusy = true;
  PersistentEventCount count;
  bool counted = visitPersistentEventLog(lowerTimestamp, upperTimestamp,
    countPersistentEvent, &count);
  if (!counted) {
    sdHistoryReaderBusy = false;
    xSemaphoreGive(sdFilesystemMutex);
    webserver_send(client, 500, (char *)(csv ? "text/csv" : "application/json"), 0);
    appendText(client, csv ? "timestamp,type,message,value\n" :
      "{\"error\":\"Could not read persistent event log\"}");
    return;
  }
  PersistentEventOutput output = {};
  output.client = client;
  output.skip = count.count > MAX_RETURNED_EVENTS ? count.count - MAX_RETURNED_EVENTS : 0;
  output.csv = csv;
  webserver_send(client, 200, (char *)(csv ? "text/csv" : "application/json"), 0);
  if (csv) appendText(client, "timestamp,type,message,value\n");
  else appendFmt(client, "{\"source\":\"sd\",\"eventCount\":%lu,\"events\":[",
    (unsigned long)count.count);
  bool written = visitPersistentEventLog(lowerTimestamp, upperTimestamp,
    appendPersistentEvent, &output);
  if (!csv) appendText(client, "]}");
  sdHistoryReaderBusy = false;
  xSemaphoreGive(sdFilesystemMutex);
  if (!written) log_message((char *)"[SD] Persistent event-log read stopped early");
}
#endif

static void pruneSdDirectory(const char *directory, time_t cutoff) {
  if (sdRetentionDays == 0) return;
  File root = SD_MMC.open(directory);
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
    if (fileTime != (time_t)-1 && fileTime < cutoff) SD_MMC.remove(name);
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
  // In 1-bit SDMMC mode the module's SPI labels map as follows:
  // CLK -> CLK, MOSI -> CMD, MISO -> D0. CS/D3 must remain pulled high.
  pinMode(HEISHAMON_SD_CS_PIN, INPUT_PULLUP);
  if (!SD_MMC.setPins(HEISHAMON_SD_SCK_PIN, HEISHAMON_SD_MOSI_PIN,
      HEISHAMON_SD_MISO_PIN) ||
      !SD_MMC.begin("/sd", true, false, HEISHAMON_SDMMC_FREQUENCY_KHZ, 5)) {
    sdState.present = false;
    sdState.filesystemOk = false;
    setSdError("No card detected or filesystem unavailable");
    return;
  }
  sdState.present = true;
  sdState.filesystemOk = true;
  sdState.active = true;
  sdState.capacity = SD_MMC.cardSize();
  sdState.freeBytes = SD_MMC.totalBytes() > SD_MMC.usedBytes() ?
    SD_MMC.totalBytes() - SD_MMC.usedBytes() : 0;
  if (!ensureSdDirectories()) return;
  cleanupSdRetention();
  char message[80];
  snprintf(message, sizeof(message), "[SD] Persistent history enabled via 1-bit SDMMC at %d kHz",
    HEISHAMON_SDMMC_FREQUENCY_KHZ);
  log_message(message);
}

static void setSdWriterError(const char *message) {
#if defined(ESP32)
  portENTER_CRITICAL(&historyDataMux);
  sdState.active = false;
  snprintf(sdState.lastError, sizeof(sdState.lastError), "%s", message);
  sdWriterErrorPending = true;
  portEXIT_CRITICAL(&historyDataMux);
#else
  setSdError(message);
#endif
}

static void setSdOpenError(const char *fileType) {
  int errorNumber = errno;
  char message[80];
  snprintf(message, sizeof(message), "Open %s failed: %d (%s)", fileType,
    errorNumber, strerror(errorNumber));
  setSdWriterError(message);
}

static bool writeSdEvents() {
  if (!sdState.active) return true;
  uint16_t localStart = 0;
  uint16_t localCount = 0;
#if defined(ESP32)
  portENTER_CRITICAL(&historyDataMux);
#endif
  localStart = eventStart;
  localCount = eventCount;
#if defined(ESP32)
  portEXIT_CRITICAL(&historyDataMux);
#endif
  if (localCount == 0) return true;
  char path[48];
  if (!sdDatePath(path, sizeof(path), "/events", "csv")) return false;
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_OPEN_EVENTS;
#endif
  FILE *file = fopen(path, "a");
  if (!file) {
    setSdOpenError("event file");
    return false;
  }
  if (ftell(file) == 0) fputs("timestamp,type,message,value\n", file);
  uint32_t newestWrittenSequence = sdFlushedEventSequence;
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_WRITE_EVENTS;
#endif
  for (uint16_t offset = 0; offset < localCount; offset++) {
    HistoryEvent event;
#if defined(ESP32)
    portENTER_CRITICAL(&historyDataMux);
#endif
    event = events[(localStart + offset) % HEISHAMON_HISTORY_MAX_EVENTS];
#if defined(ESP32)
    portEXIT_CRITICAL(&historyDataMux);
#endif
    if (event.sequence <= sdFlushedEventSequence) continue;
    fprintf(file, "%lu,%s,\"%s\",%ld\n", (unsigned long)event.timestamp,
      eventTypeName(event.type), event.message, (long)event.value);
    if (event.sequence > newestWrittenSequence) newestWrittenSequence = event.sequence;
  }
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_CLOSE_EVENTS;
#endif
  bool writeOk = ferror(file) == 0;
  if (fclose(file) != 0 || !writeOk) {
    setSdWriterError("Could not finish event file");
    return false;
  }
  sdFlushedEventSequence = newestWrittenSequence;
  return true;
}

static bool writeSdSamples() {
  if (!sdState.active) return true;
  uint16_t localStart = 0;
  uint16_t localCount = 0;
#if defined(ESP32)
  portENTER_CRITICAL(&historyDataMux);
#endif
  localStart = sampleStart;
  localCount = sampleCount;
#if defined(ESP32)
  portEXIT_CRITICAL(&historyDataMux);
#endif
  if (localCount == 0) return true;
  char path[48];
  if (!sdDatePath(path, sizeof(path), "/history", "csv")) return false;
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_OPEN_SAMPLES;
#endif
  FILE *file = fopen(path, "a");
  if (!file) {
    setSdOpenError("history file");
    return false;
  }
  if (ftell(file) == 0) fputs("timestamp,outside,inlet,outlet,target,dhw,dhw_target,room,room_target,flow,compressor_hz,pump_rpm,thermal_kw,electrical_kw,heat_production_kw,heat_consumption_kw,dhw_production_kw,dhw_consumption_kw,z1_request,z1_request_semantic,heating_curve_shift,mode,valve,state,defrost,estimated_cop\n", file);
  HistorySample oldestSample;
#if defined(ESP32)
  portENTER_CRITICAL(&historyDataMux);
#endif
  oldestSample = samples[localStart];
#if defined(ESP32)
  portEXIT_CRITICAL(&historyDataMux);
#endif
  uint32_t oldestSequence = oldestSample.sequence;
  if (sdFlushedSequence < oldestSequence - 1) sdFlushedSequence = oldestSequence - 1;
  uint32_t newestWrittenSequence = sdFlushedSequence;
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_WRITE_SAMPLES;
#endif
  for (uint16_t offset = 0; offset < localCount; offset++) {
    HistorySample sample;
#if defined(ESP32)
    portENTER_CRITICAL(&historyDataMux);
#endif
    sample = samples[(localStart + offset) % HEISHAMON_HISTORY_MAX_SAMPLES];
#if defined(ESP32)
    portEXIT_CRITICAL(&historyDataMux);
#endif
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
    fprintf(file, "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%u,%u,%u,%u,%s\n",
      (unsigned long)sample.timestamp, outside, inlet, outlet, target, dhw,
      dhwTarget, room, roomTarget, flow, hz, pump, thermal, electrical,
      heatProduction, heatConsumption, dhwProduction, dhwConsumption,
      zone1Request, zone1Semantic, curveShift,
      sample.operatingMode, sample.valveState, sample.operatingState,
      (sample.flags & SAMPLE_FLAG_DEFROST) ? 1 : 0, copText);
    if (sample.sequence > newestWrittenSequence) newestWrittenSequence = sample.sequence;
  }
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_CLOSE_SAMPLES;
#endif
  bool writeOk = ferror(file) == 0;
  if (fclose(file) != 0 || !writeOk) {
    setSdWriterError("Could not finish history file");
    return false;
  }
  sdFlushedSequence = newestWrittenSequence;
  sdState.lastWriteAt = millis();
  return true;
}

static void flushSd() {
  if (!sdState.active) return;
  writeSdSamples();
  writeSdEvents();
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

static void writeDailySummaryToSd(const DailySummary &summary) {
#if HEISHAMON_SD_HISTORY_ENABLED
  if (!summary.valid || !sdState.active) return;
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_DAILY;
#endif
  time_t value = (time_t)summary.dayStart;
  struct tm local = {};
  if (localtime_r(&value, &local) == nullptr) return;
  char path[48];
  snprintf(path, sizeof(path), "/sd/daily/%04d-%02d-%02d.csv", local.tm_year + 1900,
    local.tm_mon + 1, local.tm_mday);
  FILE *file = fopen(path, "a");
  if (!file) {
    setSdOpenError("daily summary file");
    return;
  }
  if (ftell(file) == 0) fputs("day,heating_thermal_kwh,heating_electrical_kwh,heating_cop,dhw_thermal_kwh,dhw_electrical_kwh,dhw_cop,heating_degree_days,compressor_seconds,compressor_starts,outside_min,outside_max,outside_average\n", file);
  float heatingCop = 0, dhwCop = 0;
  bool heatingValid = totalsCop(summary.heating, heatingCop);
  bool dhwValid = totalsCop(summary.dhw, dhwCop);
  double outsideAverage = summary.outsideSamples == 0 ? NAN : summary.outsideSum / summary.outsideSamples;
  fprintf(file, "%lu,%.4f,%.4f,%s,%.4f,%.4f,%s,%.4f,%lu,%lu,%s,%s,%s\n",
    (unsigned long)summary.dayStart, summary.heating.thermalKWh,
    summary.heating.electricalKWh, heatingValid ? String(heatingCop, 3).c_str() : "",
    summary.dhw.thermalKWh, summary.dhw.electricalKWh, dhwValid ? String(dhwCop, 3).c_str() : "",
    summary.heatingDegreeDays, (unsigned long)summary.compressorSeconds,
    (unsigned long)summary.compressorStarts,
    isfinite(summary.outsideMinimum) ? String(summary.outsideMinimum, 1).c_str() : "",
    isfinite(summary.outsideMaximum) ? String(summary.outsideMaximum, 1).c_str() : "",
    isfinite(outsideAverage) ? String(outsideAverage, 1).c_str() : "");
  bool writeOk = ferror(file) == 0;
  if (fclose(file) != 0 || !writeOk) {
    setSdWriterError("Could not finish daily summary file");
  }
#else
  (void)summary;
#endif
}

static void writeCycleRecordToSd(const CycleRecord &record) {
#if HEISHAMON_SD_HISTORY_ENABLED
  if (!sdState.active || record.stopTimestamp == 0) return;
#if defined(ESP32)
  sdWriterPhase = SD_PHASE_CYCLE;
#endif
  time_t value = (time_t)record.stopTimestamp;
  struct tm local = {};
  if (localtime_r(&value, &local) == nullptr) return;
  char path[48];
  snprintf(path, sizeof(path), "/sd/cycles/%04d-%02d.csv", local.tm_year + 1900,
    local.tm_mon + 1);
  FILE *file = fopen(path, "a");
  if (!file) {
    setSdOpenError("cycle summary file");
    return;
  }
  if (ftell(file) == 0) fputs("start,stop,duration_seconds,state,outside_average,flow_average,frequency_average,frequency_max,thermal_kwh,electrical_kwh,cop\n", file);
  char copText[16];
  snprintf(copText, sizeof(copText), record.copValid ? "%.3f" : "", record.cop);
  fprintf(file, "%lu,%lu,%lu,%u,%s,%s,%s,%.2f,%.4f,%.4f,%s\n",
    (unsigned long)record.startTimestamp, (unsigned long)record.stopTimestamp,
    (unsigned long)record.durationSeconds, record.operatingState,
    isfinite(record.outsideAverage) ? String(record.outsideAverage, 2).c_str() : "",
    isfinite(record.flowAverage) ? String(record.flowAverage, 2).c_str() : "",
    isfinite(record.frequencyAverage) ? String(record.frequencyAverage, 2).c_str() : "",
    record.frequencyMaximum, record.thermalKWh, record.electricalKWh, copText);
  bool writeOk = ferror(file) == 0;
  if (fclose(file) != 0 || !writeOk) {
    setSdWriterError("Could not finish cycle summary file");
  }
#else
  (void)record;
#endif
}

#if HEISHAMON_SD_HISTORY_ENABLED && defined(ESP32)
static const char *sdWriterPhaseName(SdWriterPhase phase) {
  switch (phase) {
    case SD_PHASE_OPEN_SAMPLES: return "opening history CSV";
    case SD_PHASE_WRITE_SAMPLES: return "writing history CSV";
    case SD_PHASE_CLOSE_SAMPLES: return "closing history CSV";
    case SD_PHASE_OPEN_EVENTS: return "opening events CSV";
    case SD_PHASE_WRITE_EVENTS: return "writing events CSV";
    case SD_PHASE_CLOSE_EVENTS: return "closing events CSV";
    case SD_PHASE_RETENTION: return "cleaning retained files";
    case SD_PHASE_DAILY: return "writing daily summary";
    case SD_PHASE_CYCLE: return "writing cycle summary";
    default: return "idle";
  }
}

static bool queueSdWork(const SdWorkItem &item) {
  if (!sdState.active || sdWorkQueue == nullptr) return false;
  if (xQueueSend(sdWorkQueue, &item, 0) == pdTRUE) return true;
  snprintf(sdState.lastError, sizeof(sdState.lastError),
    "SD writer queue full; record skipped");
  log_message((char *)"[SD] Writer queue full; record skipped");
  return false;
}

static void requestSdFlush() {
  if (!sdState.active || sdWorkQueue == nullptr || sdFlushQueued) return;
  SdWorkItem item = {};
  item.type = SD_WORK_FLUSH;
  sdFlushQueued = true;
  if (!queueSdWork(item)) sdFlushQueued = false;
}

static void persistDailySummary(const DailySummary &summary) {
  if (!summary.valid || !sdState.active) return;
  SdWorkItem item = {};
  item.type = SD_WORK_DAILY;
  item.daily = summary;
  queueSdWork(item);
}

static void persistCycleRecord(const CycleRecord &record) {
  if (record.stopTimestamp == 0 || !sdState.active) return;
  SdWorkItem item = {};
  item.type = SD_WORK_CYCLE;
  item.cycle = record;
  queueSdWork(item);
}

static void sdWriterTask(void *) {
  SdWorkItem item;
  while (true) {
    if (xQueueReceive(sdWorkQueue, &item, portMAX_DELAY) != pdTRUE) continue;
    if (item.type == SD_WORK_FLUSH) sdFlushQueued = false;
    sdWriterBusy = true;
    sdWriterStartedAt = millis();
    sdWriterPhase = SD_PHASE_IDLE;
    if (sdFilesystemMutex != nullptr) xSemaphoreTake(sdFilesystemMutex, portMAX_DELAY);
    switch (item.type) {
      case SD_WORK_FLUSH:
        flushSd();
        sdWriterPhase = SD_PHASE_RETENTION;
        cleanupSdRetention();
        break;
      case SD_WORK_DAILY:
        writeDailySummaryToSd(item.daily);
        break;
      case SD_WORK_CYCLE:
        writeCycleRecordToSd(item.cycle);
        break;
    }
    if (sdFilesystemMutex != nullptr) xSemaphoreGive(sdFilesystemMutex);
    sdWriterPhase = SD_PHASE_IDLE;
    sdWriterStartedAt = 0;
    sdWriterBusy = false;
  }
}

static void startSdWriter() {
  if (!sdState.active || sdWorkQueue != nullptr) return;
  if (!historyBuffersReady) {
    setSdError("PSRAM history buffers unavailable");
    return;
  }
  sdFilesystemMutex = xSemaphoreCreateMutex();
  if (sdFilesystemMutex == nullptr) {
    setSdError("Could not create SD filesystem lock");
    return;
  }
  sdWorkQueue = xQueueCreate(6, sizeof(SdWorkItem));
  if (sdWorkQueue == nullptr) {
    vSemaphoreDelete(sdFilesystemMutex);
    sdFilesystemMutex = nullptr;
    setSdError("Could not create SD writer queue");
    return;
  }
  if (xTaskCreate(sdWriterTask, "sd-history", 6144, nullptr, 1,
      &sdWriterTaskHandle) != pdPASS) {
    vQueueDelete(sdWorkQueue);
    sdWorkQueue = nullptr;
    vSemaphoreDelete(sdFilesystemMutex);
    sdFilesystemMutex = nullptr;
    setSdError("Could not start SD writer task");
    return;
  }
  log_message((char *)"[SD] Background SDMMC/POSIX writer started");
}

static void checkSdWriterHealth() {
  if (sdWriterErrorPending) {
    char error[sizeof(sdState.lastError)];
    portENTER_CRITICAL(&historyDataMux);
    snprintf(error, sizeof(error), "%s", sdState.lastError);
    sdWriterErrorPending = false;
    portEXIT_CRITICAL(&historyDataMux);
    char logLine[112];
    snprintf(logLine, sizeof(logLine), "[SD] %s", error);
    log_message(logLine);
  }
  if (!sdWriterBusy || sdHistoryReaderBusy) {
    sdWriterStallReported = false;
    return;
  }
  unsigned long startedAt = sdWriterStartedAt;
  if (startedAt == 0 || (unsigned long)(millis() - startedAt) < 15000UL ||
      sdWriterStallReported) return;
  sdWriterStallReported = true;
  char message[80];
  snprintf(message, sizeof(message), "SD writer stalled while %s; logging disabled",
    sdWriterPhaseName(sdWriterPhase));
  setSdError(message);
}
#else
static void requestSdFlush() {
  flushSd();
  cleanupSdRetention();
}

static void persistDailySummary(const DailySummary &summary) {
  writeDailySummaryToSd(summary);
}

static void persistCycleRecord(const CycleRecord &record) {
  writeCycleRecordToSd(record);
}

static void startSdWriter() {}
static void checkSdWriterHealth() {}
#endif

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
  sd["interface"] = "sdmmc-1bit";
  sd["busFrequencyKHz"] = HEISHAMON_SDMMC_FREQUENCY_KHZ;
  sd["capacityBytes"] = sdState.capacity;
  sd["freeBytes"] = sdState.freeBytes;
  if (sdState.lastWriteAt == 0) sd["lastWriteSecondsAgo"] = nullptr;
  else sd["lastWriteSecondsAgo"] = (uint32_t)((millis() - sdState.lastWriteAt) / 1000UL);
  sd["lastError"] = sdState.lastError;
#if HEISHAMON_SD_HISTORY_ENABLED && defined(ESP32)
  sd["writerBusy"] = sdWriterBusy;
  sd["writerPhase"] = sdWriterPhaseName(sdWriterPhase);
  if (!sdWriterBusy || sdWriterStartedAt == 0) sd["writerBusySeconds"] = 0;
  else sd["writerBusySeconds"] = (uint32_t)((millis() - sdWriterStartedAt) / 1000UL);
#endif
}



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
  document["sdInterface"] = "sdmmc-1bit";
  document["sdBusFrequencyKHz"] = HEISHAMON_SDMMC_FREQUENCY_KHZ;
  if (sdState.lastWriteAt == 0) document["sdLastWriteSecondsAgo"] = nullptr;
  else document["sdLastWriteSecondsAgo"] =
    (uint32_t)((millis() - sdState.lastWriteAt) / 1000UL);
  document["sdLastError"] = sdState.lastError;
#if HEISHAMON_SD_HISTORY_ENABLED && defined(ESP32)
  document["sdWriterBusy"] = sdWriterBusy;
  document["sdWriterPhase"] = sdWriterPhaseName(sdWriterPhase);
  if (!sdWriterBusy || sdWriterStartedAt == 0) document["sdWriterBusySeconds"] = 0;
  else document["sdWriterBusySeconds"] =
    (uint32_t)((millis() - sdWriterStartedAt) / 1000UL);
#endif
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
  free(samples);
  free(events);
  free(storedArchivePaths);
  samples = nullptr;
  events = nullptr;
  storedArchivePaths = nullptr;
  historyBuffersReady = false;
#if defined(ESP32)
  if (psramFound()) {
    samples = (HistorySample *)ps_malloc(sizeof(HistorySample) * HEISHAMON_HISTORY_MAX_SAMPLES);
    events = (HistoryEvent *)ps_malloc(sizeof(HistoryEvent) * HEISHAMON_HISTORY_MAX_EVENTS);
    storedArchivePaths = (char (*)[72])ps_malloc(sizeof(char) *
      MAX_STORED_HISTORY_FILES * 72);
  }
#else
  samples = (HistorySample *)malloc(sizeof(HistorySample) * HEISHAMON_HISTORY_MAX_SAMPLES);
  events = (HistoryEvent *)malloc(sizeof(HistoryEvent) * HEISHAMON_HISTORY_MAX_EVENTS);
  storedArchivePaths = (char (*)[72])malloc(sizeof(char) * MAX_STORED_HISTORY_FILES * 72);
#endif
  if (samples == nullptr || events == nullptr) {
    free(samples);
    free(events);
    samples = nullptr;
    events = nullptr;
    log_message((char *)"[HISTORY] PSRAM allocation failed; history logging disabled");
  } else {
    memset(samples, 0, sizeof(HistorySample) * HEISHAMON_HISTORY_MAX_SAMPLES);
    memset(events, 0, sizeof(HistoryEvent) * HEISHAMON_HISTORY_MAX_EVENTS);
    historyBuffersReady = true;
  }
  if (storedArchivePaths == nullptr) {
    log_message((char *)"[HISTORY] PSRAM allocation failed; SD archive reads unavailable");
  }
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
  // switchSerial() configures HeishaMon's generic GPIOs after custom feature
  // setup and would detach an already initialized SDMMC bus on GPIO34..37.
  // Defer SDMMC ownership until the first normal loop, after setup is complete.
  sdInitializationPending = true;
  char message[128];
  snprintf(message, sizeof(message),
    "[HISTORY] %s history ready: %u samples, %u bytes samples, %u bytes events, interval %u s",
#if defined(ESP32)
    historyBuffersReady ? "PSRAM" : "disabled",
#else
    historyBuffersReady ? "heap" : "disabled",
#endif
    HEISHAMON_HISTORY_MAX_SAMPLES, (unsigned)diagnosticsHistorySampleMemoryBytes(),
    (unsigned)diagnosticsHistoryEventMemoryBytes(), sampleIntervalSeconds);
  log_message(message);
}

void diagnosticsHistoryLoop() {
  if (sdInitializationPending) {
    sdInitializationPending = false;
    initializeSd();
    startSdWriter();
  }
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
  if (historyBuffersReady && dataFresh() &&
      (lastSampleAt == 0 || (unsigned long)(millis() - lastSampleAt) >= intervalMillis)) {
    HistorySample sample;
    if (makeSample(sample)) {
      sample.sequence = ++sampleSequence;
      storeSample(sample);
      updateDailySummary(samples[orderedSampleIndex(sampleCount - 1)]);
      lastSampleAt = millis();
    }
  }
  checkSdWriterHealth();
  if (sdState.active && (lastSdFlushAt == 0 ||
      (unsigned long)(millis() - lastSdFlushAt) >= 60000UL)) {
    requestSdFlush();
    lastSdFlushAt = millis();
  }
}

bool diagnosticsHistoryHandleUri(struct webserver_t *client, const char *uri) {
  if (strcmp(uri, "/diagnosticsapi") == 0 ||
      strcmp(uri, "/api/diagnostics") == 0) client->route = ROUTE_DIAGNOSTICS_API;
  else if (strcmp(uri, "/history/status") == 0 ||
      strcmp(uri, "/api/history/status") == 0) client->route = ROUTE_HISTORY_STATUS;
  else if (strcmp(uri, "/historyapi") == 0 ||
      strcmp(uri, "/api/history") == 0) client->route = ROUTE_HISTORY_API;
  else if (strcmp(uri, "/events") == 0 ||
      strcmp(uri, "/api/events") == 0) client->route = ROUTE_EVENTS_API;
  else if (strcmp(uri, "/eventlogapi") == 0 ||
      strcmp(uri, "/api/eventlog") == 0) client->route = ROUTE_PERSISTENT_EVENTS_API;
  else if (strcmp(uri, "/eventlogcsv") == 0) client->route = ROUTE_PERSISTENT_EVENTS_CSV;
  else if (strcmp(uri, "/historycommand") == 0) client->route = ROUTE_HISTORY_COMMAND;
  else if (strcmp(uri, "/cycles") == 0 || strcmp(uri, "/api/cycles") == 0) client->route = ROUTE_CYCLES_API;
  else return false;

  if (client->route == ROUTE_HISTORY_API || client->route == ROUTE_HISTORY_COMMAND ||
      client->route == ROUTE_PERSISTENT_EVENTS_API ||
      client->route == ROUTE_PERSISTENT_EVENTS_CSV) {
    HistoryRequest *request = new HistoryRequest();
    if (request == nullptr) {
      log_message((char *)"[HISTORY] Out of memory while creating request");
      client->route = 0;
      return true;
    }
    request->rangeSeconds = 0;
    request->startTimestamp = 0;
    request->endTimestamp = 0;
    request->maxPoints = 600;
    request->persistentStorage = false;
    request->response[0] = '\0';
    client->userdata = request;
  }
  return true;
}

bool diagnosticsHistoryHandleArgs(struct webserver_t *client,
    struct arguments_t *args) {
  if (client->route == ROUTE_HISTORY_API ||
      client->route == ROUTE_PERSISTENT_EVENTS_API ||
      client->route == ROUTE_PERSISTENT_EVENTS_CSV) {
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
    } else if (strcmp((char *)args->name, "start") == 0 ||
        strcmp((char *)args->name, "end") == 0) {
      if (args->len != 10) return true;
      char value[11];
      snprintf(value, sizeof(value), "%.*s", args->len, args->value);
      uint32_t timestamp = 0;
      if (parseHistoryTimestamp(value, timestamp)) {
        if (strcmp((char *)args->name, "start") == 0) request->startTimestamp = timestamp;
        else request->endTimestamp = timestamp;
      }
    } else if (strcmp((char *)args->name, "storage") == 0) {
      request->persistentStorage = args->len == 2 &&
        strncmp((char *)args->value, "sd", 2) == 0;
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
    case ROUTE_DIAGNOSTICS_API:
      if (client->content == 0) handleDiagnosticsApi(client);
      return true;
    case ROUTE_HISTORY_STATUS:
      if (client->content == 0) handleHistoryStatus(client);
      return true;
    case ROUTE_HISTORY_API: {
      if (client->content == 0) {
        HistoryRequest *request = requestContext(client);
        if (request != nullptr && request->persistentStorage) {
#if HEISHAMON_SD_HISTORY_ENABLED && defined(ESP32)
          uint32_t endTimestamp = request->endTimestamp == 0 ? currentTimestamp() :
            request->endTimestamp;
          uint32_t startTimestamp = request->startTimestamp;
          if (startTimestamp == 0 && request->rangeSeconds != 0 &&
              endTimestamp > request->rangeSeconds) {
            startTimestamp = endTimestamp - request->rangeSeconds;
          }
          handleStoredHistoryApi(client, startTimestamp, endTimestamp,
            request->maxPoints);
#else
          webserver_send(client, 503, (char *)"application/json", 0);
          appendText(client, "{\"error\":\"Persistent SD history is disabled\"}");
#endif
        } else {
          handleHistoryApi(client, request == nullptr ? 0 : request->rangeSeconds,
            request == nullptr ? 600 : request->maxPoints);
        }
        delete request;
        client->userdata = nullptr;
      }
      return true;
    }
    case ROUTE_EVENTS_API:
      if (client->content == 0) handleEventsApi(client);
      return true;
    case ROUTE_PERSISTENT_EVENTS_API:
    case ROUTE_PERSISTENT_EVENTS_CSV:
      if (client->content == 0) {
        HistoryRequest *request = requestContext(client);
#if HEISHAMON_SD_HISTORY_ENABLED && defined(ESP32)
        uint32_t endTimestamp = request == nullptr || request->endTimestamp == 0 ?
          currentTimestamp() : request->endTimestamp;
        uint32_t startTimestamp = request == nullptr ? 0 : request->startTimestamp;
        if (startTimestamp == 0 && request != nullptr && request->rangeSeconds != 0 &&
            endTimestamp > request->rangeSeconds) {
          startTimestamp = endTimestamp - request->rangeSeconds;
        }
        handlePersistentEventLog(client, startTimestamp, endTimestamp,
          client->route == ROUTE_PERSISTENT_EVENTS_CSV);
#else
        webserver_send(client, 503, (char *)(client->route == ROUTE_PERSISTENT_EVENTS_CSV ?
          "text/csv" : "application/json"), 0);
        appendText(client, client->route == ROUTE_PERSISTENT_EVENTS_CSV ?
          "timestamp,type,message,value\n" :
          "{\"error\":\"Persistent SD events are disabled\"}");
#endif
        delete request;
        client->userdata = nullptr;
      }
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
  if (client->route == ROUTE_HISTORY_API || client->route == ROUTE_HISTORY_COMMAND ||
      client->route == ROUTE_PERSISTENT_EVENTS_API ||
      client->route == ROUTE_PERSISTENT_EVENTS_CSV) {
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
  return historyBuffersReady ? sizeof(HistorySample) * HEISHAMON_HISTORY_MAX_SAMPLES : 0;
}

size_t diagnosticsHistoryEventMemoryBytes() {
  return historyBuffersReady ? sizeof(HistoryEvent) * HEISHAMON_HISTORY_MAX_EVENTS : 0;
}
