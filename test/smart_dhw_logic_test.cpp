#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "scheduler_logic.h"
#include "smart_dhw_logic.h"

static SmartDhwDecisionInput validInput(float temperature, float threshold) {
  SmartDhwDecisionInput input = {
    true, true, true, true, true, true, true, false, false, false, false,
    temperature, threshold
  };
  return input;
}

static SchedulerClock clockAt(uint16_t year, uint16_t yearDay, uint8_t weekDay,
    uint8_t hour, uint8_t minute, bool valid = true) {
  SchedulerClock clock = {valid, year, yearDay, weekDay, hour, minute};
  return clock;
}

int main() {
  // 1 and 2: evening reserve decisions.
  assert(smartDhwEvaluate(validInput(47.0f, 42.0f)) == SMART_DHW_DECISION_SUFFICIENT);
  assert(smartDhwEvaluate(validInput(39.0f, 42.0f)) == SMART_DHW_DECISION_START);

  // 3 and 4: morning reserve decisions use their own threshold.
  assert(smartDhwEvaluate(validInput(41.0f, 38.0f)) == SMART_DHW_DECISION_SUFFICIENT);
  assert(smartDhwEvaluate(validInput(34.0f, 38.0f)) == SMART_DHW_DECISION_START);

  // 5: global feature disabled.
  SmartDhwDecisionInput input = validInput(34.0f, 42.0f);
  input.enabled = false;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_DISABLED);

  // 6 and 7: either domain-specific reserve may be disabled.
  input = validInput(34.0f, 42.0f);
  input.reserveEnabled = false;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_RESERVE_DISABLED);
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_RESERVE_DISABLED);

  // 8: invalid or missing DHW temperature never starts a charge.
  input = validInput(NAN, 42.0f);
  input.temperatureValid = false;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_INVALID_TEMPERATURE);
  input = validInput(34.0f, 42.0f);
  input.stateAvailable = false;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_STATE_UNAVAILABLE);

  // 9 and 10: existing DHW/Force-DHW activity prevents duplicates.
  input = validInput(34.0f, 42.0f);
  input.dhwActive = true;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_DHW_ACTIVE);
  input = validInput(34.0f, 42.0f);
  input.forceActive = true;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_FORCE_ACTIVE);

  // 11 and 13: scheduler minute claims are unique, but work again next day.
  SchedulerClock evening = clockAt(2026, 218, 0, 18, 0);
  uint32_t key = schedulerMinuteKey(evening);
  assert(schedulerTimeMatches(true, 0x7F, 18, 0, evening, UINT32_MAX));
  assert(!schedulerTimeMatches(true, 0x7F, 18, 0, evening, key));
  SchedulerClock nextDay = clockAt(2026, 219, 1, 18, 0);
  assert(schedulerTimeMatches(true, 0x7F, 18, 0, nextDay, key));

  // 12: minimum interval and in-flight requests block starts.
  input = validInput(34.0f, 42.0f);
  input.minimumIntervalBlocked = true;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_MINIMUM_INTERVAL);
  input = validInput(34.0f, 42.0f);
  input.controllerBusy = true;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_CONTROLLER_BUSY);

  // 14: invalid local time cannot produce a scheduled action.
  input = validInput(34.0f, 42.0f);
  input.clockValid = false;
  assert(smartDhwEvaluate(input) == SMART_DHW_DECISION_INVALID_CLOCK);
  SchedulerClock invalidClock = clockAt(1970, 0, 3, 18, 0, false);
  assert(!schedulerTimeMatches(true, 0x7F, 18, 0, invalidClock, UINT32_MAX));

  // Invalid configuration is rejected before any command path is reached.
  assert(smartDhwConfigValuesValid(18, 0, 42.0f, 4, 0, 38.0f, 60));
  assert(!smartDhwConfigValuesValid(24, 0, 42.0f, 4, 0, 38.0f, 60));
  assert(!smartDhwConfigValuesValid(18, 0, 10.0f, 4, 0, 38.0f, 60));
  assert(!smartDhwConfigValuesValid(18, 0, 42.0f, 4, 0, 38.0f, 1));

  puts("smart_dhw_logic_test: all checks passed");
  return 0;
}
