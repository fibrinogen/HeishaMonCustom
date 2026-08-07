#pragma once

#include <math.h>

enum SmartDhwDecisionCode : unsigned char {
  SMART_DHW_DECISION_START = 0,
  SMART_DHW_DECISION_SUFFICIENT,
  SMART_DHW_DECISION_DISABLED,
  SMART_DHW_DECISION_RESERVE_DISABLED,
  SMART_DHW_DECISION_INVALID_CLOCK,
  SMART_DHW_DECISION_INVALID_CONFIG,
  SMART_DHW_DECISION_STATE_UNAVAILABLE,
  SMART_DHW_DECISION_NOT_INSTALLED,
  SMART_DHW_DECISION_INVALID_TEMPERATURE,
  SMART_DHW_DECISION_DHW_ACTIVE,
  SMART_DHW_DECISION_FORCE_ACTIVE,
  SMART_DHW_DECISION_CONTROLLER_BUSY,
  SMART_DHW_DECISION_MINIMUM_INTERVAL
};

struct SmartDhwDecisionInput {
  bool enabled;
  bool reserveEnabled;
  bool clockValid;
  bool configValid;
  bool stateAvailable;
  bool dhwInstalled;
  bool temperatureValid;
  bool dhwActive;
  bool forceActive;
  bool controllerBusy;
  bool minimumIntervalBlocked;
  float temperature;
  float threshold;
};

inline bool smartDhwConfigValuesValid(unsigned char eveningHour,
    unsigned char eveningMinute, float eveningTemperature,
    unsigned char morningHour, unsigned char morningMinute,
    float morningTemperature, unsigned short minimumIntervalMinutes) {
  return eveningHour <= 23 && eveningMinute <= 59 && morningHour <= 23 &&
    morningMinute <= 59 && isfinite(eveningTemperature) &&
    eveningTemperature >= 20.0f && eveningTemperature <= 75.0f &&
    isfinite(morningTemperature) && morningTemperature >= 20.0f &&
    morningTemperature <= 75.0f && minimumIntervalMinutes >= 15 &&
    minimumIntervalMinutes <= 1440;
}

inline SmartDhwDecisionCode smartDhwEvaluate(const SmartDhwDecisionInput &input) {
  if (!input.enabled) return SMART_DHW_DECISION_DISABLED;
  if (!input.reserveEnabled) return SMART_DHW_DECISION_RESERVE_DISABLED;
  if (!input.clockValid) return SMART_DHW_DECISION_INVALID_CLOCK;
  if (!input.configValid) return SMART_DHW_DECISION_INVALID_CONFIG;
  if (!input.stateAvailable) return SMART_DHW_DECISION_STATE_UNAVAILABLE;
  if (!input.dhwInstalled) return SMART_DHW_DECISION_NOT_INSTALLED;
  if (!input.temperatureValid || !isfinite(input.temperature)) {
    return SMART_DHW_DECISION_INVALID_TEMPERATURE;
  }
  if (input.dhwActive) return SMART_DHW_DECISION_DHW_ACTIVE;
  if (input.forceActive) return SMART_DHW_DECISION_FORCE_ACTIVE;
  if (input.controllerBusy) return SMART_DHW_DECISION_CONTROLLER_BUSY;
  if (input.minimumIntervalBlocked) return SMART_DHW_DECISION_MINIMUM_INTERVAL;
  if (input.temperature >= input.threshold) return SMART_DHW_DECISION_SUFFICIENT;
  return SMART_DHW_DECISION_START;
}
