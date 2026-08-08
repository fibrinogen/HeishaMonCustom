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
  uint16_t uptimeSeconds;
  int16_t outsideTemp10;
  int16_t inletTemp10;
  int16_t outletTemp10;
  int16_t targetTemp10;
  int16_t dhwTemp10;
  uint16_t flow100;
  uint16_t compressorHz10;
  uint16_t pumpRpm;
  int16_t thermalPower100;
  uint8_t operatingMode;
  uint8_t valveState;
  uint8_t flags;
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
  HISTORY_FIELD_EXTERNAL_0 = 1u << 9,
  HISTORY_FIELD_EXTERNAL_1 = 1u << 10,
  HISTORY_FIELD_EXTERNAL_2 = 1u << 11,
  HISTORY_FIELD_EXTERNAL_3 = 1u << 12,
  HISTORY_FIELD_EXTERNAL_4 = 1u << 13,
  HISTORY_FIELD_EXTERNAL_5 = 1u << 14,
  HISTORY_FIELD_EXTERNAL_6 = 1u << 15,
  HISTORY_FIELD_EXTERNAL_7 = 1u << 16
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
  HISTORY_EVENT_SMART_DHW,
  HISTORY_EVENT_COMMUNICATION,
  HISTORY_EVENT_MQTT,
  HISTORY_EVENT_OPERATION_MODE_CHANGED,
  HISTORY_EVENT_COUNT
};

struct HistoryEvent {
  uint32_t sequence;
  uint32_t timestamp;
  uint32_t uptimeSeconds;
  int32_t value;
  HistoryEventType type;
  uint8_t timeValid;
  char message[48];
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
