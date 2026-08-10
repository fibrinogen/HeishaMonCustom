#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum HeatingCurveShiftImplementation : uint8_t {
  HEATING_CURVE_SHIFT_UNAVAILABLE = 0,
  HEATING_CURVE_SHIFT_NATIVE_TOP27,
  HEATING_CURVE_SHIFT_CURVE_ENDPOINTS
};

struct HeatingCurveShiftStatus {
  bool available;
  bool valueValid;
  bool baselineInitialized;
  bool externalMismatch;
  int8_t requestedShift;
  int8_t minShift;
  int8_t maxShift;
  uint8_t heatingMode;
  uint8_t sensorSetting;
  HeatingCurveShiftImplementation implementation;
  int16_t baseTargetHigh;
  int16_t baseTargetLow;
  int16_t outsideHigh;
  int16_t outsideLow;
  int16_t effectiveTargetHigh;
  int16_t effectiveTargetLow;
  int16_t panasonicTargetHigh;
  int16_t panasonicTargetLow;
};

bool heatingCurveShiftGetStatus(HeatingCurveShiftStatus *status);
void heatingCurveShiftToJson(JsonObject object);
bool heatingCurveShiftSet(int value, char *response, size_t responseSize);
bool heatingCurveShiftSetBase(bool high, int value, char *response,
  size_t responseSize);
void heatingCurveShiftLoop();
