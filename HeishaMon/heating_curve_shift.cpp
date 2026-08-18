#include "heating_curve_shift.h"

#include <cstdlib>
#include <cstring>

#include "commands.h"
#include "webfunctions.h"

#define DATASIZE 203

extern char actData[DATASIZE];
extern unsigned long lastHeatpumpDataAt;
extern settingsStruct heishamonSettings;
extern bool send_command(byte *command, int length);
extern String getDataValue(char *data, unsigned int topicNumber);
extern void log_message(char *string);

static constexpr uint8_t TOP27 = 27;
static constexpr uint8_t TOP29 = 29;
static constexpr uint8_t TOP30 = 30;
static constexpr uint8_t TOP31 = 31;
static constexpr uint8_t TOP32 = 32;
static constexpr uint8_t TOP76 = 76;
static constexpr int16_t CURVE_VALUE_MIN = -50;
static constexpr int16_t CURVE_VALUE_MAX = 100;

static bool freshFrame() {
  if (actData[0] != 0x71 || actData[1] != (char)0xC8 ||
      actData[2] != 0x01 || actData[3] != 0x10 || lastHeatpumpDataAt == 0) {
    return false;
  }
  unsigned long maximumAge = (unsigned long)heishamonSettings.waitTime * 4000UL;
  if (maximumAge < 60000UL) maximumAge = 60000UL;
  return (unsigned long)(millis() - lastHeatpumpDataAt) <= maximumAge;
}

static bool readInteger(uint8_t topic, int *value) {
  if (value == nullptr) return false;
  String text = getDataValue(actData, topic);
  if (text.length() == 0) return false;
  char *end = nullptr;
  long parsed = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || parsed < -32768 || parsed > 32767) return false;
  *value = (int)parsed;
  return true;
}

static bool readCurveValue(uint8_t topic, int16_t *value) {
  int parsed = 0;
  if (value == nullptr || !readInteger(topic, &parsed) ||
      parsed < CURVE_VALUE_MIN || parsed > CURVE_VALUE_MAX) return false;
  *value = (int16_t)parsed;
  return true;
}

static bool curveFrame(int16_t *targetAtCold, int16_t *targetAtWarm,
    int16_t *outsideWarm, int16_t *outsideCold) {
  return readCurveValue(TOP29, targetAtCold) &&
    readCurveValue(TOP30, targetAtWarm) &&
    readCurveValue(TOP31, outsideWarm) &&
    readCurveValue(TOP32, outsideCold);
}

static bool targetPairValid(int16_t targetAtCold, int16_t targetAtWarm) {
  return targetAtCold >= CURVE_VALUE_MIN && targetAtCold <= CURVE_VALUE_MAX &&
    targetAtWarm >= CURVE_VALUE_MIN && targetAtWarm <= CURVE_VALUE_MAX &&
    targetAtCold >= targetAtWarm;
}

static void statusDefaults(HeatingCurveShiftStatus *status) {
  memset(status, 0, sizeof(*status));
  status->minShift = -5;
  status->maxShift = 5;
  status->heatingMode = UINT8_MAX;
}

static bool sendCurveTargets(int16_t targetAtCold, int16_t targetAtWarm,
    int16_t outsideWarm, int16_t outsideCold, char *response,
    size_t responseSize) {
  if (!targetPairValid(targetAtCold, targetAtWarm)) {
    snprintf(response, responseSize,
      "Heating curve target values are outside the supported range");
    return false;
  }
  if (outsideWarm < CURVE_VALUE_MIN || outsideWarm > CURVE_VALUE_MAX ||
      outsideCold < CURVE_VALUE_MIN || outsideCold > CURVE_VALUE_MAX ||
      outsideCold >= outsideWarm) {
    snprintf(response, responseSize,
      "Heating curve cold outside temperature must be below the warm temperature");
    return false;
  }

  int16_t currentTargetAtCold = 0;
  int16_t currentTargetAtWarm = 0;
  int16_t currentOutsideWarm = 0;
  int16_t currentOutsideCold = 0;
  if (curveFrame(&currentTargetAtCold, &currentTargetAtWarm,
      &currentOutsideWarm, &currentOutsideCold) &&
      currentTargetAtCold == targetAtCold &&
      currentTargetAtWarm == targetAtWarm &&
      currentOutsideWarm == outsideWarm &&
      currentOutsideCold == outsideCold) {
    snprintf(response, responseSize,
      "Zone 1 heating curve already has requested values");
    return true;
  }

  JsonDocument document;
  document["zone1"]["heat"]["target"]["high"] = targetAtCold;
  document["zone1"]["heat"]["target"]["low"] = targetAtWarm;
  // SetCurves describes the outside endpoints as high=warm and low=cold.
  document["zone1"]["heat"]["outside"]["high"] = outsideWarm;
  document["zone1"]["heat"]["outside"]["low"] = outsideCold;
  String payload;
  serializeJson(document, payload);

  unsigned char command[256] = {0};
  char commandLog[256] = {0};
  unsigned int length = set_curves((char *)payload.c_str(), command, commandLog);
  if (length == 0 || !send_command(command, length)) {
    snprintf(response, responseSize, "SetCurves command queue rejected");
    return false;
  }
  snprintf(response, responseSize, "SetCurves: %s", commandLog);
  log_message(commandLog);
  return true;
}

bool heatingCurveShiftGetStatus(HeatingCurveShiftStatus *status) {
  if (status == nullptr) return false;
  statusDefaults(status);
  if (!freshFrame()) return false;

  int mode = 0;
  if (!readInteger(TOP76, &mode) || (mode != 0 && mode != 1)) return false;
  status->heatingMode = (uint8_t)mode;
  if (mode != 0) return true;

  int raw = 0;
  if (!readInteger(TOP27, &raw) || raw < -128 || raw > 127) return false;
  status->available = true;
  status->requestedShift = (int8_t)raw;
  status->valueValid = raw >= status->minShift && raw <= status->maxShift;
  return true;
}

void heatingCurveShiftToJson(JsonObject object) {
  HeatingCurveShiftStatus status;
  bool resolved = heatingCurveShiftGetStatus(&status);
  object["available"] = resolved && status.available;
  object["valueValid"] = resolved && status.valueValid;
  object["implementation"] = resolved && status.available ?
    "nativeTop27" : "unavailable";
  object["implementationLabel"] = resolved && status.available ?
    "Panasonic TOP27 shift" : "Unavailable";
  if (resolved && status.available) {
    object["shift"] = status.requestedShift;
    object["rawValue"] = status.requestedShift;
  } else {
    object["shift"] = nullptr;
    object["rawValue"] = nullptr;
  }
  object["min"] = status.minShift;
  object["max"] = status.maxShift;
  object["step"] = 1;
  // An out-of-range native value remains writable so a valid write can recover it.
  object["writable"] = resolved && status.available;
  if (resolved && status.heatingMode != UINT8_MAX) {
    object["heatingMode"] = status.heatingMode;
  } else {
    object["heatingMode"] = nullptr;
  }
}

bool heatingCurveShiftSet(int value, char *response, size_t responseSize) {
  if (response == nullptr || responseSize == 0) return false;
  if (heishamonSettings.listenonly) {
    snprintf(response, responseSize, "Listen-only mode");
    return false;
  }
  if (value < -5 || value > 5) {
    snprintf(response, responseSize,
      "Heating curve shift must be between -5 and 5 K");
    return false;
  }

  HeatingCurveShiftStatus status;
  if (!heatingCurveShiftGetStatus(&status) || !status.available) {
    snprintf(response, responseSize,
      "Heating curve shift is unavailable outside compensation-curve mode");
    return false;
  }
  if (status.valueValid && status.requestedShift == value) {
    snprintf(response, responseSize,
      "Heating curve shift already has requested value %d K", value);
    return true;
  }

  char valueText[16];
  snprintf(valueText, sizeof(valueText), "%d", value);
  unsigned char command[256] = {0};
  char commandLog[256] = {0};
  unsigned int length = set_z1_heat_request_temperature(
    valueText, command, commandLog);
  if (length == 0 || !send_command(command, length)) {
    snprintf(response, responseSize, "TOP27 command queue rejected");
    return false;
  }
  snprintf(response, responseSize, "Heating curve shift queued: %d K", value);
  log_message(commandLog);
  return true;
}

bool heatingCurveSettingsSet(int targetAtCold, int targetAtWarm,
    int outsideCold, int outsideWarm, char *response, size_t responseSize) {
  if (response == nullptr || responseSize == 0) return false;
  if (heishamonSettings.listenonly) {
    snprintf(response, responseSize, "Listen-only mode");
    return false;
  }
  if (!freshFrame()) {
    snprintf(response, responseSize,
      "Fresh heat-pump curve data is unavailable");
    return false;
  }
  if (!targetPairValid(targetAtCold, targetAtWarm)) {
    snprintf(response, responseSize,
      "Cold-weather water target must be at least the warm-weather target");
    return false;
  }
  if (outsideCold < CURVE_VALUE_MIN || outsideCold > CURVE_VALUE_MAX ||
      outsideWarm < CURVE_VALUE_MIN || outsideWarm > CURVE_VALUE_MAX ||
      outsideCold >= outsideWarm) {
    snprintf(response, responseSize,
      "Cold outside temperature must be below the warm outside temperature");
    return false;
  }
  if (!sendCurveTargets(targetAtCold, targetAtWarm, outsideWarm, outsideCold,
      response, responseSize)) return false;
  if (strstr(response, "already has requested values") != nullptr) return true;
  snprintf(response, responseSize,
    "Zone 1 heating curve queued: %d C at %d C outside, %d C at %d C outside",
    targetAtCold, outsideCold, targetAtWarm, outsideWarm);
  return true;
}
