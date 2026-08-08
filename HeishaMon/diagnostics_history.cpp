#include "diagnostics_history.h"

#include "decode.h"
#include "diagnostics_logic.h"
#include "history_config.h"
#include "version.h"
#include "webfunctions.h"

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

namespace {

constexpr uint8_t ROUTE_DIAGNOSTICS = 27;
constexpr uint8_t ROUTE_DIAGNOSTICS_API = 28;
constexpr uint8_t ROUTE_HISTORY = 29;
constexpr uint8_t ROUTE_HISTORY_STATUS = 30;
constexpr uint8_t ROUTE_HISTORY_API = 31;
constexpr uint8_t ROUTE_EVENTS_API = 32;
constexpr uint8_t ROUTE_HISTORY_COMMAND = 33;

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

constexpr uint8_t SAMPLE_FLAG_COMPRESSOR = 0x01;
constexpr uint8_t SAMPLE_FLAG_HEATPUMP = 0x02;
constexpr uint8_t SAMPLE_FLAG_DHW = 0x04;
constexpr uint8_t SAMPLE_FLAG_DEFROST = 0x08;
constexpr uint8_t SAMPLE_FLAG_TIME_VALID = 0x10;

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
  unsigned long compressorStartedAt = 0;
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
  char buffer[512];
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

  if (!cycle.initialized) {
    cycle.initialized = true;
    cycle.compressorRunning = compressorRunning;
    cycle.heatpumpOn = heatpumpOn;
    cycle.dhwActive = dhwActive;
    cycle.defrostActive = defrostActive;
    cycle.valve = valve;
    cycle.operationMode = operationMode;
    cycle.errorActive = errorActive;
    if (compressorRunning) cycle.compressorStartedAt = millis();
    return;
  }

  if (compressorRunning != cycle.compressorRunning) {
    if (compressorRunning) {
      cycle.compressorStartedAt = millis();
      cycle.startsSinceBoot++;
      addEvent(HISTORY_EVENT_COMPRESSOR_START, "Compressor started");
    } else {
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
}

static bool makeSample(HistorySample &sample) {
  if (!dataFresh()) return false;
  memset(&sample, 0, sizeof(sample));
  bool timestampValid = false;
  sample.timestamp = currentTimestamp(&timestampValid);
  sample.uptimeSeconds = (uint16_t)(millis() / 1000UL);
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
  if (readTopic(TOP_OPERATION_MODE, value)) sample.operatingMode = (uint8_t)max(0L, min(255L, lroundf(value)));
  if (readTopic(TOP_VALVE, value)) sample.valveState = (uint8_t)max(0L, min(255L, lroundf(value)));
  if (readTopic(TOP_HEATPUMP_STATE, value) && lroundf(value) != 0) sample.flags |= SAMPLE_FLAG_HEATPUMP;
  if (readNonNegative(TOP_COMPRESSOR_HZ, value) && value > 0.5f) sample.flags |= SAMPLE_FLAG_COMPRESSOR;
  if (currentDhwActive()) sample.flags |= SAMPLE_FLAG_DHW;
  if (readTopic(TOP_DEFROST, value) && lroundf(value) != 0) sample.flags |= SAMPLE_FLAG_DEFROST;
  if (timestampValid) sample.flags |= SAMPLE_FLAG_TIME_VALID;

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
  if (end == value || *end != '\0' || parsed < 5 || parsed > 60) return 0;
  if (parsed != 5 && parsed != 10 && parsed != 30 && parsed != 60) return 0;
  return (uint16_t)parsed;
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

static bool sampleInRange(const HistorySample &sample, uint32_t rangeSeconds) {
  if (rangeSeconds == 0) return true;
  bool timeValid = (sample.flags & SAMPLE_FLAG_TIME_VALID) != 0 && validClock();
  uint32_t now = currentTimestamp();
  uint32_t sampleTime = timeValid ? sample.timestamp : sample.uptimeSeconds;
  if (!timeValid) now = (uint32_t)(millis() / 1000UL);
  return now >= sampleTime && now - sampleTime <= rangeSeconds;
}

static uint16_t orderedSampleIndex(uint16_t offset) {
  return (uint16_t)((sampleStart + offset) % HEISHAMON_HISTORY_MAX_SAMPLES);
}

struct SampleAggregate {
  HistorySample sample;
  float sums[9];
  uint16_t counts[9];
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
  aggregate.sample.validFields |= sample.validFields;
  memcpy(aggregate.sample.externalValues, sample.externalValues,
    sizeof(aggregate.sample.externalValues));
  const uint32_t fields[] = {
    HISTORY_FIELD_OUTSIDE, HISTORY_FIELD_INLET, HISTORY_FIELD_OUTLET,
    HISTORY_FIELD_TARGET, HISTORY_FIELD_DHW, HISTORY_FIELD_FLOW,
    HISTORY_FIELD_COMPRESSOR_HZ, HISTORY_FIELD_PUMP_RPM,
    HISTORY_FIELD_THERMAL_POWER
  };
  const float values[] = {
    sample.outsideTemp10 / 10.0f, sample.inletTemp10 / 10.0f,
    sample.outletTemp10 / 10.0f, sample.targetTemp10 / 10.0f,
    sample.dhwTemp10 / 10.0f, sample.flow100 / 100.0f,
    sample.compressorHz10 / 10.0f, (float)sample.pumpRpm,
    sample.thermalPower100 / 100.0f
  };
  for (uint8_t i = 0; i < 9; i++) {
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
    HISTORY_FIELD_THERMAL_POWER
  };
  for (uint8_t i = 0; i < 9; i++) {
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
    }
  }
}

static void appendSampleJson(struct webserver_t *client, const HistorySample &sample) {
  char outside[20], inlet[20], outlet[20], target[20], dhw[20];
  char flow[20], hz[20], pump[20], power[20];
  snprintf(outside, sizeof(outside), (sample.validFields & HISTORY_FIELD_OUTSIDE) ? "%.1f" : "null", sample.outsideTemp10 / 10.0f);
  snprintf(inlet, sizeof(inlet), (sample.validFields & HISTORY_FIELD_INLET) ? "%.1f" : "null", sample.inletTemp10 / 10.0f);
  snprintf(outlet, sizeof(outlet), (sample.validFields & HISTORY_FIELD_OUTLET) ? "%.1f" : "null", sample.outletTemp10 / 10.0f);
  snprintf(target, sizeof(target), (sample.validFields & HISTORY_FIELD_TARGET) ? "%.1f" : "null", sample.targetTemp10 / 10.0f);
  snprintf(dhw, sizeof(dhw), (sample.validFields & HISTORY_FIELD_DHW) ? "%.1f" : "null", sample.dhwTemp10 / 10.0f);
  snprintf(flow, sizeof(flow), (sample.validFields & HISTORY_FIELD_FLOW) ? "%.2f" : "null", sample.flow100 / 100.0f);
  snprintf(hz, sizeof(hz), (sample.validFields & HISTORY_FIELD_COMPRESSOR_HZ) ? "%.1f" : "null", sample.compressorHz10 / 10.0f);
  snprintf(pump, sizeof(pump), (sample.validFields & HISTORY_FIELD_PUMP_RPM) ? "%u" : "null", sample.pumpRpm);
  snprintf(power, sizeof(power), (sample.validFields & HISTORY_FIELD_THERMAL_POWER) ? "%.2f" : "null", sample.thermalPower100 / 100.0f);
  appendFmt(client,
    "{\"t\":%lu,\"u\":%lu,\"outside\":%s,\"inlet\":%s,\"outlet\":%s,\"target\":%s,\"dhw\":%s,\"flow\":%s,\"hz\":%s,\"pump\":%s,\"power\":%s,\"mode\":%u,\"valve\":%u,\"compressor\":%s,\"timeValid\":%s}",
    (unsigned long)sample.timestamp, (unsigned long)sample.uptimeSeconds,
    outside, inlet, outlet, target, dhw, flow, hz, pump, power,
    sample.operatingMode,
    sample.valveState, (sample.flags & SAMPLE_FLAG_COMPRESSOR) ? "true" : "false",
    (sample.flags & SAMPLE_FLAG_TIME_VALID) ? "true" : "false");
}

static void loadHistoryConfig() {
  if (!LittleFS.begin() || !LittleFS.exists("/history.json")) return;
  File file = LittleFS.open("/history.json", "r");
  if (!file) return;
  JsonDocument document;
  if (!deserializeJson(document, file)) {
    uint16_t configured = (uint16_t)(document["intervalSeconds"] | 0);
    if (configured == 5 || configured == 10 || configured == 30 || configured == 60) {
      sampleIntervalSeconds = configured;
    }
    uint16_t retention = (uint16_t)(document["retentionDays"] | sdRetentionDays);
    if (retention == 0 || retention == 7 || retention == 14 || retention == 30 ||
        retention == 90) sdRetentionDays = retention;
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
  bool ok = serializeJson(document, file) > 0;
  file.close();
  return ok;
}

#if HEISHAMON_SD_HISTORY_ENABLED
static SPIClass sdSpi(FSPI);

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

static void writeSdEvents() {
  if (!sdState.active || eventCount == 0) return;
  char path[48];
  if (!sdDatePath(path, sizeof(path), "/events", "csv")) return;
  bool exists = SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    setSdError("Could not open event file");
    return;
  }
  if (!exists) file.println("timestamp,type,message,value");
  for (uint16_t offset = 0; offset < eventCount; offset++) {
    const HistoryEvent &event = events[(eventStart + offset) % HEISHAMON_HISTORY_MAX_EVENTS];
    if (event.sequence <= sdFlushedEventSequence) continue;
    file.printf("%lu,%s,\"%s\",%ld\n", (unsigned long)event.timestamp,
      eventTypeName(event.type), event.message, (long)event.value);
  }
  file.close();
}

static void writeSdSamples() {
  if (!sdState.active || sampleCount == 0) return;
  char path[48];
  if (!sdDatePath(path, sizeof(path), "/history", "csv")) return;
  bool exists = SD.exists(path);
  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    setSdError("Could not open history file");
    return;
  }
  if (!exists) file.println("timestamp,outside,inlet,outlet,target,dhw,flow,compressor_hz,thermal_kw,mode,valve");
  uint32_t oldestSequence = sampleSequence >= sampleCount ?
    sampleSequence - sampleCount + 1 : 1;
  if (sdFlushedSequence < oldestSequence - 1) sdFlushedSequence = oldestSequence - 1;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    const HistorySample &sample = samples[orderedSampleIndex(offset)];
    if (sample.sequence <= sdFlushedSequence) continue;
    file.printf("%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.1f,%.2f,%u,%u\n",
      (unsigned long)sample.timestamp, sample.outsideTemp10 / 10.0f,
      sample.inletTemp10 / 10.0f, sample.outletTemp10 / 10.0f,
      sample.targetTemp10 / 10.0f, sample.dhwTemp10 / 10.0f,
      sample.flow100 / 100.0f, sample.compressorHz10 / 10.0f,
      sample.thermalPower100 / 100.0f, sample.operatingMode, sample.valveState);
  }
  file.close();
  sdState.lastWriteAt = millis();
}

static void flushSd() {
  if (!sdState.active || (sampleCount == 0 && eventCount == 0)) return;
  writeSdSamples();
  sdFlushedSequence = sampleSequence;
  writeSdEvents();
  sdFlushedEventSequence = eventSequence;
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
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Diagnostics - HeishaMon</title><style>
:root{font-family:Arial,sans-serif;color:#17202a;background:#f4f6f8}body{margin:0;padding:18px}.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}.top a{color:#168dcc;text-decoration:none;margin-left:12px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:12px}.card{background:#fff;border:1px solid #dfe4e8;border-radius:10px;padding:14px;box-shadow:0 1px 3px #0001}.card h2{font-size:16px;color:#159bd2;margin:0 0 10px}.row{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px solid #edf0f2;font-size:13px}.row:last-child{border-bottom:0}.value{font-weight:600;text-align:right}.ok{color:#159b68}.bad{color:#df334d}.muted{color:#7a8791}.raw{margin-top:12px}.raw summary{cursor:pointer;font-weight:600}.raw pre{max-height:360px;overflow:auto;background:#17202a;color:#eef;padding:10px;border-radius:6px;font-size:11px}@media(prefers-color-scheme:dark){:root{color:#eef;background:#101319}.card{background:#181d26;border-color:#303846}.row{border-color:#29313d}.raw pre{background:#0b0e13}}
</style></head><body><div class='top'><h1>Diagnostics</h1><nav><a href='/'>Home</a><a href='/dashboard'>Dashboard</a><a href='/history'>History</a><a href='/wpsettings'>Settings</a></nav></div>
<div id='status' class='muted'>Loading…</div><div id='cards' class='grid'></div><details class='card raw'><summary>Raw Panasonic values</summary><pre id='raw'>Loading…</pre></details>
<script>
function esc(v){return String(v==null?'N/A':v).replace(/[&<>"']/g,function(c){return({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c]})}
function val(v,unit){return v===null||v===undefined?'N/A':esc(v)+(unit||'')}
function row(k,v){return '<div class="row"><span>'+esc(k)+'</span><span class="value">'+v+'</span></div>'}
function card(title,rows){return '<section class="card"><h2>'+title+'</h2>'+rows.join('')+'</section>'}
function render(d){var s=d.system||{},o=d.operation||{},h=d.hydraulics||{},c=d.control||{},w=d.dhw||{},n=d.counters||{},sd=d.sd||{};
 document.getElementById('status').innerHTML='Panasonic: <b class="'+(s.panasonic&&s.panasonic.fresh?'ok':'bad')+'">'+esc(s.panasonic?s.panasonic.status:'NO DATA')+'</b> · Last frame: '+val(s.panasonic&&s.panasonic.ageSeconds,' s');
 document.getElementById('cards').innerHTML=card('SYSTEM STATUS',[row('WiFi',s.wifi?'Connected':'Offline'),row('Ethernet',s.ethernet?'Connected':'Offline'),row('MQTT',s.mqtt?'Connected':'Offline'),row('NTP',s.ntp?'Synchronized':'Unavailable'),row('Uptime',val(s.uptimeSeconds,' s')),row('Free heap',val(s.freeHeap,' B')),row('PSRAM',s.psramFound?val(s.freePsram,' B free'):'Not available'),row('Firmware',val(s.firmware))])+card('CURRENT OPERATION',[row('Interpreted state',val(o.state)),row('Operating mode',val(o.mode)),row('Compressor',o.compressorRunning?'Running':'Stopped'),row('Compressor frequency',val(o.compressorFrequency,' Hz')),row('Current runtime',val(o.currentRuntimeSeconds,' s')),row('Flow',val(o.flow,' l/min')),row('Pump speed',val(o.pumpSpeed,' rpm')),row('Fan 1',val(o.fan1,' rpm')),row('3-way valve',val(o.valve))])+card('HYDRAULICS',[row('Inlet',val(h.inlet,' °C')),row('Outlet',val(h.outlet,' °C')),row('Target',val(h.target,' °C')),row('Delta T',val(h.deltaT,' K')),row('Target error',val(h.targetError,' K')),row('Delta-T error',val(h.deltaTError,' K')),row('Calculated thermal power',val(h.calculatedThermalPowerKw,' kW'))])+card('CONTROL / DHW',[row('Room temperature',val(c.roomTemperature,' °C')),row('Heating mode',val(c.heatingMode)),row('Heating-off outdoor temp',val(c.heatingOffOutside,' °C')),row('DHW actual',val(w.actual,' °C')),row('DHW target',val(w.target,' °C')),row('DHW active',w.active?'Yes':'No'),row('Force DHW',val(w.forceState)),row('Smart DHW','See Smart DHW page')])+card('COUNTERS',[row('Panasonic operation hours',val(n.panasonicOperationHours)),row('Panasonic operation counter',val(n.panasonicOperationCounter)),row('Compressor starts since boot',val(n.compressorStartsSinceBoot)),row('Current cycle',val(n.currentCycleSeconds,' s')),row('Previous cycle',val(n.previousCycleSeconds,' s')),row('Average cycle',val(n.averageCycleSeconds,' s'))])+card('PERSISTENT HISTORY',[row('RAM samples',val((d.history||{}).sampleCount)+' / '+val((d.history||{}).capacity)),row('Sample interval',val((d.history||{}).intervalSeconds,' s')),row('RAM memory',val((d.history||{}).sampleMemoryBytes,' B')),row('SD support',sd.support?'Enabled':'Disabled'),row('SD card',sd.present?'Present':'Not present'),row('History logging',sd.active?'Active':'RAM only'),row('Retention',sd.retentionDays===0?'Unlimited':val(sd.retentionDays,' days')),row('SD error',val(sd.lastError))]);
 document.getElementById('raw').textContent=JSON.stringify(d,null,2);
}
function refresh(){fetch('/diagnosticsapi',{cache:'no-store'}).then(function(r){if(!r.ok)throw Error(r.status);return r.json()}).then(render).catch(function(e){document.getElementById('status').textContent='Diagnostics unavailable: '+e.message})}refresh();setInterval(refresh,5000);
</script></body></html>)HTML";

static const char historyPage[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>History - HeishaMon</title><style>
:root{font-family:Arial,sans-serif;color:#17202a;background:#f4f6f8}body{margin:0;padding:18px}.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}.top a{color:#168dcc;text-decoration:none;margin-left:12px}.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:12px}.toolbar button,.toolbar select{padding:7px 10px;border:1px solid #ccd5dc;border-radius:6px;background:#fff}.card{background:#fff;border:1px solid #dfe4e8;border-radius:10px;padding:14px;margin-bottom:12px}.chart{width:100%;height:230px}.legend{font-size:12px;color:#697680}.events{max-height:250px;overflow:auto}.event{padding:6px 0;border-bottom:1px solid #edf0f2;font-size:13px}@media(prefers-color-scheme:dark){:root{color:#eef;background:#101319}.card,.toolbar button,.toolbar select{background:#181d26;color:#eef;border-color:#303846}.event{border-color:#29313d}}
</style></head><body><div class='top'><h1>History</h1><nav><a href='/'>Home</a><a href='/dashboard'>Dashboard</a><a href='/diagnostics'>Diagnostics</a><a href='/wpsettings'>Settings</a></nav></div>
<div class='toolbar'><label>Range <select id='range' onchange='refresh()'><option value='30m'>Last 30 min</option><option value='1h' selected>Last 1 h</option><option value='3h'>Last 3 h</option><option value='all'>All RAM history</option></select></label><label>Sample interval <select id='interval' onchange='setIntervalValue()'><option>5</option><option selected>10</option><option>30</option><option>60</option></select> s</label><label>SD retention <select id='retention' onchange='setRetentionValue()'><option value='0'>Unlimited</option><option value='7'>7 days</option><option value='14'>14 days</option><option value='30' selected>30 days</option><option value='90'>90 days</option></select></label><span id='status' class='legend'>Loading…</span></div>
<section class='card'><h2>Temperatures</h2><canvas id='temps' class='chart'></canvas><div class='legend'>Outlet · Inlet · Target · Outside</div></section><section class='card'><h2>Compressor / hydraulics</h2><canvas id='hydraulics' class='chart'></canvas><div class='legend'>Compressor frequency · Flow · Calculated thermal power</div></section><section class='card'><h2>DHW</h2><canvas id='dhw' class='chart'></canvas><div class='legend'>DHW actual · DHW target</div></section><section class='card'><h2>Events</h2><div id='events' class='events'></div></section>
<script>
var colors=['#159bd2','#df334d','#e39b22','#159b68','#8756d6'];
function draw(id,points,lines){var c=document.getElementById(id),ctx=c.getContext('2d'),w=c.clientWidth||800,h=230,d=devicePixelRatio||1;c.width=w*d;c.height=h*d;ctx.scale(d,d);ctx.clearRect(0,0,w,h);if(!points.length)return;var values=[];lines.forEach(function(l){points.forEach(function(p){if(p[l.key]!==null)values.push(Number(p[l.key]))})});if(!values.length)return;var min=Math.min.apply(null,values),max=Math.max.apply(null,values);if(min===max){min-=1;max+=1}function x(i){return 10+(w-25)*(i/(points.length-1||1))}function y(v){return h-18-(h-35)*(v-min)/(max-min)};ctx.font='11px Arial';ctx.fillStyle='#75818a';ctx.fillText(max.toFixed(1),4,14);ctx.fillText(min.toFixed(1),4,h-5);lines.forEach(function(l,li){ctx.strokeStyle=colors[li%colors.length];ctx.lineWidth=2;ctx.beginPath();var started=false;points.forEach(function(p,i){if(p[l.key]===null)return;var px=x(i),py=y(Number(p[l.key]));if(!started){ctx.moveTo(px,py);started=true}else ctx.lineTo(px,py)});ctx.stroke()})}
function render(d){var p=d.samples||[];draw('temps',p,[{key:'outlet'},{key:'inlet'},{key:'target'},{key:'outside'}]);draw('hydraulics',p,[{key:'hz'},{key:'flow'},{key:'power'}]);draw('dhw',p,[{key:'dhw'},{key:'target'}]);var e=d.events||[];document.getElementById('events').innerHTML=e.length?e.slice().reverse().map(function(x){var stamp=x.timeValid?new Date((x.t||0)*1000).toLocaleString():'uptime '+(x.u||0)+' s';return '<div class="event"><b>'+stamp+'</b> '+x.type+': '+x.message+'</div>'}).join(''):'No events';document.getElementById('status').textContent=p.length+' samples · '+(d.intervalSeconds||0)+' s interval'}
function refresh(){fetch('/historyapi?range='+encodeURIComponent(document.getElementById('range').value),{cache:'no-store'}).then(function(r){if(!r.ok)throw Error(r.status);return r.json()}).then(render).catch(function(e){document.getElementById('status').textContent='History unavailable: '+e.message})}
function setIntervalValue(){fetch('/historycommand?interval='+document.getElementById('interval').value).then(refresh)}
function setRetentionValue(){fetch('/historycommand?retention='+document.getElementById('retention').value).then(refresh)}
fetch('/history/status').then(function(r){return r.json()}).then(function(s){document.getElementById('interval').value=String(s.intervalSeconds||10);document.getElementById('retention').value=String(s.sdRetentionDays===undefined?30:s.sdRetentionDays)}).catch(function(){}).then(refresh);setInterval(refresh,10000);window.addEventListener('resize',refresh);
</script></body></html>)HTML";

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
  webserver_send_content_P(client, diagnosticsPage, strlen_P(diagnosticsPage));
}

static void handleHistoryPage(struct webserver_t *client) {
  webserver_send(client, 200, (char *)"text/html", 0);
  webserver_send_content_P(client, historyPage, strlen_P(historyPage));
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
  sendJsonDocument(client, document);
}

static void handleHistoryApi(struct webserver_t *client, uint32_t rangeSeconds,
    uint16_t maxPoints) {
  webserver_send(client, 200, (char *)"application/json", 0);
  appendFmt(client, "{\"intervalSeconds\":%u,\"sampleCount\":%u,\"samples\":[",
    sampleIntervalSeconds, sampleCount);
  uint16_t matching = 0;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    if (sampleInRange(samples[orderedSampleIndex(offset)], rangeSeconds)) matching++;
  }
  uint16_t step = matching > maxPoints ? (uint16_t)((matching + maxPoints - 1) / maxPoints) : 1;
  uint16_t matchedIndex = 0;
  SampleAggregate aggregate;
  resetAggregate(aggregate);
  bool first = true;
  for (uint16_t offset = 0; offset < sampleCount; offset++) {
    const HistorySample &sample = samples[orderedSampleIndex(offset)];
    if (!sampleInRange(sample, rangeSeconds)) continue;
    addAggregate(aggregate, sample);
    matchedIndex++;
    if (aggregate.count >= step || matchedIndex == matching) {
      finishAggregate(aggregate);
      if (!first) appendText(client, ",");
      first = false;
      appendSampleJson(client, aggregate.sample);
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
      if (parsed == 0) snprintf(request->response, sizeof(request->response), "ERROR: interval must be 5, 10, 30 or 60 seconds");
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
