#include "heating_curve_shift.h"

#include <cmath>
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
static constexpr uint8_t TOP111 = 111;
static constexpr int16_t CURVE_VALUE_MIN = -50;
static constexpr int16_t CURVE_VALUE_MAX = 100;
static constexpr unsigned long CURVE_WRITE_SETTLE_MS = 60000UL;

static bool startupSyncAttempted = false;
static bool curveWritePending = false;
static unsigned long curveWriteQueuedAt = 0;
static int16_t pendingTargetHigh = 0;
static int16_t pendingTargetLow = 0;
static int16_t pendingOutsideHigh = 0;
static int16_t pendingOutsideLow = 0;

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

static bool curveFrame(int16_t *high, int16_t *low, int16_t *outsideHigh,
    int16_t *outsideLow) {
  return readCurveValue(TOP29, high) && readCurveValue(TOP30, low) &&
    readCurveValue(TOP31, outsideHigh) && readCurveValue(TOP32, outsideLow);
}

static bool targetPairValid(int16_t high, int16_t low, int shift) {
  int effectiveHigh = high + shift;
  int effectiveLow = low + shift;
  return effectiveHigh >= CURVE_VALUE_MIN && effectiveHigh <= CURVE_VALUE_MAX &&
    effectiveLow >= CURVE_VALUE_MIN && effectiveLow <= CURVE_VALUE_MAX &&
    effectiveHigh >= effectiveLow;
}

static void persistCurveSettings() {
  JsonDocument document;
  settingsToJson(document, &heishamonSettings);
  saveJsonToFile(document, "/config.json");
}

static void statusDefaults(HeatingCurveShiftStatus *status) {
  memset(status, 0, sizeof(*status));
  status->implementation = HEATING_CURVE_SHIFT_UNAVAILABLE;
  status->minShift = -5;
  status->maxShift = 5;
}

static bool readMode(HeatingCurveShiftStatus *status) {
  int mode = 0;
  int sensor = 0;
  if (!readInteger(TOP76, &mode) || !readInteger(TOP111, &sensor) ||
      (mode != 0 && mode != 1) || sensor < 0 || sensor > 3) return false;
  status->heatingMode = (uint8_t)mode;
  status->sensorSetting = (uint8_t)sensor;
  // A curve shift is meaningful only while Panasonic uses the compensation
  // curve. TOP27 is native shift only for water-temperature control; with a
  // room sensor TOP27 remains the room request and the curve endpoints move.
  if (mode != 0) return false;
  status->implementation = sensor == 0 ? HEATING_CURVE_SHIFT_NATIVE_TOP27 :
    HEATING_CURVE_SHIFT_CURVE_ENDPOINTS;
  return true;
}

static bool ensureBaseline() {
  if (heishamonSettings.wpCurveBaselineValid) {
    return targetPairValid(heishamonSettings.wpCurveBaseHigh,
      heishamonSettings.wpCurveBaseLow, heishamonSettings.wpCurveShift) &&
      heishamonSettings.wpCurveOutsideHigh >= CURVE_VALUE_MIN &&
      heishamonSettings.wpCurveOutsideHigh <= CURVE_VALUE_MAX &&
      heishamonSettings.wpCurveOutsideLow >= CURVE_VALUE_MIN &&
      heishamonSettings.wpCurveOutsideLow <= CURVE_VALUE_MAX &&
      heishamonSettings.wpCurveOutsideLow < heishamonSettings.wpCurveOutsideHigh;
  }

  int16_t high = 0, low = 0, outsideHigh = 0, outsideLow = 0;
  if (!curveFrame(&high, &low, &outsideHigh, &outsideLow) || high < low) return false;
  heishamonSettings.wpCurveBaseHigh = high;
  heishamonSettings.wpCurveBaseLow = low;
  heishamonSettings.wpCurveOutsideHigh = outsideHigh;
  heishamonSettings.wpCurveOutsideLow = outsideLow;
  heishamonSettings.wpCurveShift = 0;
  heishamonSettings.wpCurveBaselineValid = true;
  persistCurveSettings();
  return true;
}

static bool sendCurveTargets(int16_t high, int16_t low, int16_t outsideHigh,
    int16_t outsideLow, char *response, size_t responseSize) {
  if (!targetPairValid(high, low, 0)) {
    snprintf(response, responseSize, "Heating curve target values are outside the safe range");
    return false;
  }
  if (outsideHigh < CURVE_VALUE_MIN || outsideHigh > CURVE_VALUE_MAX ||
      outsideLow < CURVE_VALUE_MIN || outsideLow > CURVE_VALUE_MAX ||
      outsideLow >= outsideHigh) {
    snprintf(response, responseSize,
      "Heating curve cold outside temperature must be below the warm temperature");
    return false;
  }
  JsonDocument document;
  document["zone1"]["heat"]["target"]["high"] = high;
  document["zone1"]["heat"]["target"]["low"] = low;
  // The SetCurves names describe outside temperature: high is the warm
  // endpoint (TOP31) and low is the cold endpoint (TOP32).
  document["zone1"]["heat"]["outside"]["high"] = outsideHigh;
  document["zone1"]["heat"]["outside"]["low"] = outsideLow;
  String payload;
  serializeJson(document, payload);

  unsigned char command[256] = {0};
  char commandLog[256] = {0};
  unsigned int length = set_curves((char *)payload.c_str(), command, commandLog);
  if (length == 0 || !send_command(command, length)) {
    snprintf(response, responseSize, "SetCurves command queue rejected");
    return false;
  }
  curveWritePending = true;
  curveWriteQueuedAt = millis();
  pendingTargetHigh = high;
  pendingTargetLow = low;
  pendingOutsideHigh = outsideHigh;
  pendingOutsideLow = outsideLow;
  snprintf(response, responseSize, "SetCurves: %s", commandLog);
  log_message(commandLog);
  return true;
}

bool heatingCurveShiftGetStatus(HeatingCurveShiftStatus *status) {
  if (status == nullptr) return false;
  statusDefaults(status);
  if (!freshFrame() || !readMode(status)) return false;

  if (status->implementation == HEATING_CURVE_SHIFT_NATIVE_TOP27) {
    int raw = 0;
    if (!readInteger(TOP27, &raw)) return false;
    status->available = true;
    status->requestedShift = (int8_t)raw;
    status->valueValid = raw >= -5 && raw <= 5;
    return true;
  }

  if (!ensureBaseline()) return false;
  status->available = true;
  status->baselineInitialized = true;
  status->requestedShift = heishamonSettings.wpCurveShift;
  status->baseTargetHigh = heishamonSettings.wpCurveBaseHigh;
  status->baseTargetLow = heishamonSettings.wpCurveBaseLow;
  status->outsideHigh = heishamonSettings.wpCurveOutsideHigh;
  status->outsideLow = heishamonSettings.wpCurveOutsideLow;
  status->effectiveTargetHigh = status->baseTargetHigh + status->requestedShift;
  status->effectiveTargetLow = status->baseTargetLow + status->requestedShift;
  status->valueValid = targetPairValid(status->baseTargetHigh,
    status->baseTargetLow, status->requestedShift);
  int16_t currentHigh = 0, currentLow = 0, currentOutsideHigh = 0, currentOutsideLow = 0;
  if (curveFrame(&currentHigh, &currentLow, &currentOutsideHigh, &currentOutsideLow)) {
    bool pendingMatches = currentHigh == pendingTargetHigh &&
      currentLow == pendingTargetLow && currentOutsideHigh == pendingOutsideHigh &&
      currentOutsideLow == pendingOutsideLow;
    if (curveWritePending && pendingMatches) curveWritePending = false;
    bool pendingSettling = curveWritePending &&
      (unsigned long)(millis() - curveWriteQueuedAt) <= CURVE_WRITE_SETTLE_MS;
    if (curveWritePending && !pendingSettling) curveWritePending = false;
    // With no requested shift, a deliberate Panasonic curve edit is the new
    // base. Once a non-zero shift is active, never absorb the effective values
    // back into the base because that would cause cumulative drift. A frame
    // received just after our own write may still contain the old values.
    if (!pendingSettling && status->requestedShift == 0 && currentHigh >= currentLow &&
        (currentHigh != status->baseTargetHigh || currentLow != status->baseTargetLow ||
         currentOutsideHigh != status->outsideHigh || currentOutsideLow != status->outsideLow)) {
      heishamonSettings.wpCurveBaseHigh = currentHigh;
      heishamonSettings.wpCurveBaseLow = currentLow;
      heishamonSettings.wpCurveOutsideHigh = currentOutsideHigh;
      heishamonSettings.wpCurveOutsideLow = currentOutsideLow;
      status->baseTargetHigh = currentHigh;
      status->baseTargetLow = currentLow;
      status->outsideHigh = currentOutsideHigh;
      status->outsideLow = currentOutsideLow;
      persistCurveSettings();
    }
    status->panasonicTargetHigh = currentHigh;
    status->panasonicTargetLow = currentLow;
    status->externalMismatch = !pendingSettling &&
      (currentHigh != status->effectiveTargetHigh ||
       currentLow != status->effectiveTargetLow);
  }
  return true;
}

void heatingCurveShiftToJson(JsonObject object) {
  HeatingCurveShiftStatus status;
  bool resolved = heatingCurveShiftGetStatus(&status);
  object["available"] = resolved && status.available;
  object["valueValid"] = resolved && status.valueValid;
  object["implementation"] = status.implementation == HEATING_CURVE_SHIFT_NATIVE_TOP27 ?
    "nativeTop27" : status.implementation == HEATING_CURVE_SHIFT_CURVE_ENDPOINTS ?
    "curveEndpoints" : "unavailable";
  object["implementationLabel"] = status.implementation == HEATING_CURVE_SHIFT_NATIVE_TOP27 ?
    "Panasonic TOP27 shift" : status.implementation == HEATING_CURVE_SHIFT_CURVE_ENDPOINTS ?
    "Curve endpoint adjustment" : "Unavailable";
  if (resolved && status.available) object["shift"] = status.requestedShift;
  else object["shift"] = nullptr;
  if (resolved && status.available &&
      status.implementation == HEATING_CURVE_SHIFT_NATIVE_TOP27) {
    object["rawValue"] = status.requestedShift;
  } else {
    object["rawValue"] = nullptr;
  }
  object["min"] = status.minShift;
  object["max"] = status.maxShift;
  object["step"] = 1;
  // A known native TOP27 shift remains writable even when Panasonic reports
  // an out-of-range raw value; an explicit valid write is the recovery path.
  object["writable"] = resolved && status.available &&
    (status.implementation == HEATING_CURVE_SHIFT_NATIVE_TOP27 || status.valueValid);
  object["baselineInitialized"] = status.baselineInitialized;
  object["externalMismatch"] = status.externalMismatch;
  if (status.implementation == HEATING_CURVE_SHIFT_CURVE_ENDPOINTS) {
    object["baseTargetHigh"] = status.baseTargetHigh;
    object["baseTargetLow"] = status.baseTargetLow;
    object["effectiveTargetHigh"] = status.effectiveTargetHigh;
    object["effectiveTargetLow"] = status.effectiveTargetLow;
    object["panasonicTargetHigh"] = status.panasonicTargetHigh;
    object["panasonicTargetLow"] = status.panasonicTargetLow;
    object["outsideHigh"] = status.outsideHigh;
    object["outsideLow"] = status.outsideLow;
  }
  object["heatingMode"] = status.heatingMode;
  object["sensorSetting"] = status.sensorSetting;
}

bool heatingCurveShiftSet(int value, char *response, size_t responseSize) {
  if (response == nullptr || responseSize == 0) return false;
  if (heishamonSettings.listenonly) {
    snprintf(response, responseSize, "Listen-only mode");
    return false;
  }
  if (value < -5 || value > 5) {
    snprintf(response, responseSize, "Heating curve shift must be between -5 and 5 K");
    return false;
  }
  HeatingCurveShiftStatus status;
  if (!heatingCurveShiftGetStatus(&status) || !status.available) {
    snprintf(response, responseSize, "Heating curve shift is unavailable for this configuration");
    return false;
  }
  if (status.implementation == HEATING_CURVE_SHIFT_NATIVE_TOP27) {
    char valueText[16];
    snprintf(valueText, sizeof(valueText), "%d", value);
    unsigned char command[256] = {0};
    char commandLog[256] = {0};
    unsigned int length = set_z1_heat_request_temperature(valueText, command, commandLog);
    if (length == 0 || !send_command(command, length)) {
      snprintf(response, responseSize, "TOP27 command queue rejected");
      return false;
    }
    snprintf(response, responseSize, "Heating curve shift queued: %d K", value);
    log_message(commandLog);
    return true;
  }

  int16_t high = heishamonSettings.wpCurveBaseHigh + value;
  int16_t low = heishamonSettings.wpCurveBaseLow + value;
  if (!targetPairValid(heishamonSettings.wpCurveBaseHigh,
      heishamonSettings.wpCurveBaseLow, value)) {
    snprintf(response, responseSize, "Requested shift would exceed heating curve limits");
    return false;
  }
  if (!sendCurveTargets(high, low, heishamonSettings.wpCurveOutsideHigh,
      heishamonSettings.wpCurveOutsideLow, response, responseSize)) return false;
  heishamonSettings.wpCurveShift = (int8_t)value;
  persistCurveSettings();
  return true;
}

bool heatingCurveShiftSetBase(bool high, int value, char *response,
    size_t responseSize) {
  if (response == nullptr || responseSize == 0) return false;
  HeatingCurveShiftStatus status;
  if (heishamonSettings.listenonly || !heatingCurveShiftGetStatus(&status) ||
      status.implementation != HEATING_CURVE_SHIFT_CURVE_ENDPOINTS) {
    snprintf(response, responseSize, "Heating curve base is unavailable for this configuration");
    return false;
  }
  if (value < CURVE_VALUE_MIN || value > CURVE_VALUE_MAX) {
    snprintf(response, responseSize, "Heating curve base must be between %d and %d C",
      CURVE_VALUE_MIN, CURVE_VALUE_MAX);
    return false;
  }
  int16_t newHigh = high ? value : heishamonSettings.wpCurveBaseHigh;
  int16_t newLow = high ? heishamonSettings.wpCurveBaseLow : value;
  if (!targetPairValid(newHigh, newLow, heishamonSettings.wpCurveShift)) {
    snprintf(response, responseSize, "Heating curve base would make the curve invalid");
    return false;
  }
  if (!sendCurveTargets(newHigh + heishamonSettings.wpCurveShift,
      newLow + heishamonSettings.wpCurveShift,
      heishamonSettings.wpCurveOutsideHigh, heishamonSettings.wpCurveOutsideLow,
      response, responseSize)) return false;
  heishamonSettings.wpCurveBaseHigh = newHigh;
  heishamonSettings.wpCurveBaseLow = newLow;
  persistCurveSettings();
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
    snprintf(response, responseSize, "Fresh heat-pump curve data is unavailable");
    return false;
  }
  if (!targetPairValid(targetAtCold, targetAtWarm, 0)) {
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

  HeatingCurveShiftStatus status;
  bool endpointShift = heatingCurveShiftGetStatus(&status) && status.available &&
    status.implementation == HEATING_CURVE_SHIFT_CURVE_ENDPOINTS;
  int shift = endpointShift ? status.requestedShift : 0;
  if (!targetPairValid(targetAtCold, targetAtWarm, shift)) {
    snprintf(response, responseSize,
      "Heating curve plus the active shift exceeds the supported range");
    return false;
  }
  if (!sendCurveTargets(targetAtCold + shift, targetAtWarm + shift,
      outsideWarm, outsideCold, response, responseSize)) return false;

  heishamonSettings.wpCurveBaseHigh = targetAtCold;
  heishamonSettings.wpCurveBaseLow = targetAtWarm;
  heishamonSettings.wpCurveOutsideHigh = outsideWarm;
  heishamonSettings.wpCurveOutsideLow = outsideCold;
  heishamonSettings.wpCurveShift = endpointShift ? (int8_t)shift : 0;
  heishamonSettings.wpCurveBaselineValid = true;
  persistCurveSettings();
  snprintf(response, responseSize,
    "Zone 1 heating curve queued: %d C at %d C outside, %d C at %d C outside",
    targetAtCold, outsideCold, targetAtWarm, outsideWarm);
  return true;
}

void heatingCurveShiftLoop() {
  if (startupSyncAttempted) return;
  HeatingCurveShiftStatus status;
  if (!heatingCurveShiftGetStatus(&status) ||
      status.implementation != HEATING_CURVE_SHIFT_CURVE_ENDPOINTS) return;
  // Mark the first fresh comparison as complete even when no recovery is
  // needed. This prevents a later manual Panasonic edit from being overwritten.
  if (!status.externalMismatch || status.requestedShift == 0) {
    startupSyncAttempted = true;
    return;
  }
  char response[128] = {0};
  // One guarded recovery after a fresh, normal frame. A later manual
  // Panasonic change is reported as a mismatch but is never rewritten in a loop.
  startupSyncAttempted = true;
  sendCurveTargets(status.effectiveTargetHigh, status.effectiveTargetLow,
    status.outsideHigh, status.outsideLow, response, sizeof(response));
}
