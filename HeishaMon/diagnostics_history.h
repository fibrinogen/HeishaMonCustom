#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "src/common/webserver.h"
#include "history_config.h"

// Compact, fixed-size sample. Temperatures are stored in 0.1 °C, flow in
// 0.01 l/min, compressor frequency in 0.1 Hz and power in 0.01 kW.
struct HistorySample {
  uint32_t sequence;
  uint32_t timestamp;
  uint32_t uptimeSeconds;
  int16_t outsideTemp10;
  int16_t inletTemp10;
  int16_t outletTemp10;
  int16_t targetTemp10;
  int16_t dhwTemp10;
  int16_t dhwTargetTemp10;
  int16_t roomTemp10;
  int16_t roomTarget10;
  uint16_t flow100;
  uint16_t compressorHz10;
  uint16_t pumpRpm;
  int16_t thermalPower100;
  uint16_t electricalPowerW;
  uint16_t heatProductionW;
  uint16_t heatConsumptionW;
  uint16_t dhwProductionW;
  uint16_t dhwConsumptionW;
  uint8_t operatingMode;
  uint8_t valveState;
  uint8_t flags;
  uint8_t operatingState;
  uint8_t electricalSource;
  int16_t zone1RequestValue10;
  uint8_t zone1RequestSemantic;
  int8_t heatingCurveShift;
  uint32_t validFields;
  float externalValues[HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX];
};

enum HistorySampleField : uint32_t {
  HISTORY_FIELD_OUTSIDE = 1u << 0,
  HISTORY_FIELD_INLET = 1u << 1,
  HISTORY_FIELD_OUTLET = 1u << 2,
  HISTORY_FIELD_TARGET = 1u << 3,
  HISTORY_FIELD_DHW = 1u << 4,
  HISTORY_FIELD_FLOW = 1u << 5,
  HISTORY_FIELD_COMPRESSOR_HZ = 1u << 6,
  HISTORY_FIELD_PUMP_RPM = 1u << 7,
  HISTORY_FIELD_THERMAL_POWER = 1u << 8,
  HISTORY_FIELD_ELECTRICAL_POWER = 1u << 9,
  HISTORY_FIELD_DHW_TARGET = 1u << 10,
  HISTORY_FIELD_ROOM = 1u << 11,
  HISTORY_FIELD_ROOM_TARGET = 1u << 12,
  HISTORY_FIELD_HEAT_PRODUCTION = 1u << 13,
  HISTORY_FIELD_HEAT_CONSUMPTION = 1u << 14,
  HISTORY_FIELD_DHW_PRODUCTION = 1u << 15,
  HISTORY_FIELD_DHW_CONSUMPTION = 1u << 16,
  HISTORY_FIELD_EXTERNAL_0 = 1u << 17,
  HISTORY_FIELD_EXTERNAL_1 = 1u << 18,
  HISTORY_FIELD_EXTERNAL_2 = 1u << 19,
  HISTORY_FIELD_EXTERNAL_3 = 1u << 20,
  HISTORY_FIELD_EXTERNAL_4 = 1u << 21,
  HISTORY_FIELD_EXTERNAL_5 = 1u << 22,
  HISTORY_FIELD_EXTERNAL_6 = 1u << 23,
  HISTORY_FIELD_EXTERNAL_7 = 1u << 24,
  HISTORY_FIELD_ZONE1_REQUEST = 1u << 25,
  HISTORY_FIELD_HEATING_CURVE_SHIFT = 1u << 26
};

enum HistoryOperatingState : uint8_t {
  HISTORY_STATE_UNKNOWN = 0,
  HISTORY_STATE_STANDBY,
  HISTORY_STATE_HEATING,
  HISTORY_STATE_DHW,
  HISTORY_STATE_DEFROST,
  HISTORY_STATE_CIRCULATION,
  HISTORY_STATE_TRANSITION
};

enum HistoryEventType : uint8_t {
  HISTORY_EVENT_COMPRESSOR_START = 0,
  HISTORY_EVENT_COMPRESSOR_STOP,
  HISTORY_EVENT_HEATPUMP_ON,
  HISTORY_EVENT_HEATPUMP_OFF,
  HISTORY_EVENT_DHW_START,
  HISTORY_EVENT_DHW_STOP,
  HISTORY_EVENT_DEFROST_START,
  HISTORY_EVENT_DEFROST_STOP,
  HISTORY_EVENT_VALVE_CHANGED,
  HISTORY_EVENT_ERROR_APPEARED,
  HISTORY_EVENT_ERROR_CLEARED,
  HISTORY_EVENT_SCHEDULER,
  // Retain this slot so events stored by older firmware keep their numeric type.
  HISTORY_EVENT_LEGACY_SMART_DHW,
  HISTORY_EVENT_COMMUNICATION,
  HISTORY_EVENT_MQTT,
  HISTORY_EVENT_OPERATION_MODE_CHANGED,
  HISTORY_EVENT_ZONE1_SEMANTIC_CHANGED,
  HISTORY_EVENT_SYSTEM,
  HISTORY_EVENT_COUNT
};

struct HistoryEvent {
  uint32_t sequence;
  uint32_t timestamp;
  uint32_t uptimeSeconds;
  int32_t value;
  HistoryEventType type;
  uint8_t timeValid;
  char message[96];
};

void diagnosticsHistoryBegin();
void diagnosticsHistoryLoop();

bool diagnosticsHistoryHandleUri(struct webserver_t *client, const char *uri);
bool diagnosticsHistoryHandleArgs(struct webserver_t *client,
  struct arguments_t *args);
bool diagnosticsHistoryHandleWrite(struct webserver_t *client);
bool diagnosticsHistoryHandleClose(struct webserver_t *client);

void diagnosticsHistoryRecordEvent(HistoryEventType type, const char *message,
  int32_t value = 0);

size_t diagnosticsHistorySampleMemoryBytes();
size_t diagnosticsHistoryEventMemoryBytes();
