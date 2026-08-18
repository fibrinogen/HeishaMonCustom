#include "zone1_heat_semantics.h"

#include <cmath>
#include <cstdlib>

#include "decode.h"

static const uint8_t ZONE1_HEAT_REQUEST_TOPIC = 27;
static const uint8_t ZONE1_HEATING_MODE_TOPIC = 76;

static bool parseFinite(const String &text, float *value) {
  if (value == nullptr || text.length() == 0) return false;
  const char *start = text.c_str();
  char *end = nullptr;
  float parsed = strtof(start, &end);
  if (end == start || *end != '\0' || !isfinite(parsed)) return false;
  *value = parsed;
  return true;
}

static bool readIntegerTopic(char *data, uint8_t topic, int *value) {
  float parsed = 0;
  if (value == nullptr || !parseFinite(getDataValue(data, topic), &parsed) ||
      parsed != lroundf(parsed)) return false;
  *value = (int)parsed;
  return true;
}

const char *zone1HeatRequestSemanticName(Zone1HeatRequestSemanticType type) {
  switch (type) {
    case ZONE1_HEAT_CURVE_SHIFT: return "heatCurveShift";
    case ZONE1_HEATING_WATER_TARGET: return "heatingWaterTarget";
    case ZONE1_ROOM_TARGET: return "roomTarget";
    default: return "unknown";
  }
}

bool readZone1HeatRequestRaw(char *data, float *value) {
  if (data == nullptr || value == nullptr || data[0] != 0x71 ||
      data[1] != (char)0xC8 || data[2] != 0x01 || data[3] != 0x10) return false;
  return parseFinite(getDataValue(data, ZONE1_HEAT_REQUEST_TOPIC), value);
}

bool resolveZone1HeatRequestSemantic(char *data, int16_t waterMin,
    int16_t waterMax, Zone1HeatRequestSemantic *result) {
  if (result == nullptr) return false;
  *result = {
    ZONE1_HEAT_SEMANTIC_UNKNOWN,
    "unknown",
    "Zone 1 request",
    "",
    0,
    0,
    1,
    false,
    -1
  };
  if (data == nullptr || data[0] != 0x71 || data[1] != (char)0xC8 ||
      data[2] != 0x01 || data[3] != 0x10 || waterMin > waterMax) return false;

  int heatingMode = 0;
  if (!readIntegerTopic(data, ZONE1_HEATING_MODE_TOPIC, &heatingMode) ||
      (heatingMode != 0 && heatingMode != 1)) return false;

  result->heatingMode = (int8_t)heatingMode;
  result->step = 1;
  result->writable = true;

  // Panasonic defines TOP27 by the Zone 1 heating mode: it is the native
  // parallel shift in compensation-curve mode and an absolute requested water
  // temperature in direct mode. The selected zone sensor does not change this
  // protocol field into a room setpoint.
  if (heatingMode == 0) {
    result->type = ZONE1_HEAT_CURVE_SHIFT;
    result->name = zone1HeatRequestSemanticName(result->type);
    result->label = "Heating curve shift";
    result->unit = "K";
    result->minValue = -5;
    result->maxValue = 5;
  } else {
    result->type = ZONE1_HEATING_WATER_TARGET;
    result->name = zone1HeatRequestSemanticName(result->type);
    result->label = "Heating water target";
    result->unit = "\xC2\xB0" "C";
    result->minValue = waterMin;
    result->maxValue = waterMax;
  }
  return true;
}

bool zone1HeatRequestValueValid(const Zone1HeatRequestSemantic &semantic,
    float value) {
  return semantic.type != ZONE1_HEAT_SEMANTIC_UNKNOWN && semantic.writable &&
    isfinite(value) && value >= semantic.minValue && value <= semantic.maxValue &&
    value == lroundf(value);
}

void zone1HeatRequestSemanticToJson(JsonObject object, char *data,
    int16_t waterMin, int16_t waterMax) {
  Zone1HeatRequestSemantic semantic;
  bool resolved = resolveZone1HeatRequestSemantic(data, waterMin, waterMax, &semantic);
  float raw = 0;
  bool rawValid = readZone1HeatRequestRaw(data, &raw);

  object["available"] = data != nullptr && data[0] != '\0';
  if (rawValid) object["rawValue"] = raw;
  else object["rawValue"] = nullptr;
  object["semanticKnown"] = resolved;
  object["semantic"] = zone1HeatRequestSemanticName(semantic.type);
  object["label"] = semantic.label;
  object["unit"] = semantic.unit;
  object["min"] = semantic.minValue;
  object["max"] = semantic.maxValue;
  object["step"] = semantic.step;
  object["writable"] = resolved && semantic.writable;
  object["heatingMode"] = semantic.heatingMode;
  object["heatingModeLabel"] = semantic.heatingMode == 0 ? "Compensation curve" :
    semantic.heatingMode == 1 ? "Direct" : "Unknown";
  bool valueValid = resolved && rawValid && zone1HeatRequestValueValid(semantic, raw);
  if (valueValid) object["value"] = raw;
  else object["value"] = nullptr;
  object["rawValidForSemantic"] = valueValid;
}
