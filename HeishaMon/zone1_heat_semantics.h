#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum Zone1HeatRequestSemanticType : uint8_t {
  ZONE1_HEAT_SEMANTIC_UNKNOWN = 0,
  ZONE1_HEAT_CURVE_SHIFT,
  ZONE1_HEATING_WATER_TARGET,
  ZONE1_ROOM_TARGET
};

struct Zone1HeatRequestSemantic {
  Zone1HeatRequestSemanticType type;
  const char *name;
  const char *label;
  const char *unit;
  int16_t minValue;
  int16_t maxValue;
  int16_t step;
  bool writable;
  int8_t heatingMode;
  int8_t sensorSetting;
};

bool resolveZone1HeatRequestSemantic(char *data, int16_t waterMin,
  int16_t waterMax, Zone1HeatRequestSemantic *result);
bool readZone1HeatRequestRaw(char *data, float *value);
bool zone1HeatRequestValueValid(const Zone1HeatRequestSemantic &semantic,
  float value);
const char *zone1HeatRequestSemanticName(Zone1HeatRequestSemanticType type);

void zone1HeatRequestSemanticToJson(JsonObject object, char *data,
  int16_t waterMin, int16_t waterMax);
