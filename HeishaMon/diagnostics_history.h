#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "src/common/webserver.h"
#include "history_config.h"

#include "history_sample.h"

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
void diagnosticsHistoryMqttConnected();

bool diagnosticsHistoryHandleUri(struct webserver_t *client, const char *uri);
bool diagnosticsHistoryHandleArgs(struct webserver_t *client,
  struct arguments_t *args);
bool diagnosticsHistoryHandleWrite(struct webserver_t *client);
bool diagnosticsHistoryHandleClose(struct webserver_t *client);

void diagnosticsHistoryRecordEvent(HistoryEventType type, const char *message,
  int32_t value = 0);

size_t diagnosticsHistorySampleMemoryBytes();
size_t diagnosticsHistoryEventMemoryBytes();
