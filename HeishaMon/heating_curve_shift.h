#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct HeatingCurveShiftStatus {
  bool available;
  bool valueValid;
  int8_t requestedShift;
  int8_t minShift;
  int8_t maxShift;
  uint8_t heatingMode;
};

bool heatingCurveShiftGetStatus(HeatingCurveShiftStatus *status);
void heatingCurveShiftToJson(JsonObject object);
bool heatingCurveShiftSet(int value, char *response, size_t responseSize);
bool heatingCurveSettingsSet(int targetAtCold, int targetAtWarm,
  int outsideCold, int outsideWarm, char *response, size_t responseSize);
