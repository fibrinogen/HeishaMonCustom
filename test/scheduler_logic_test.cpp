#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "scheduler_logic.h"

static SchedulerClock clockAt(uint16_t year, uint16_t yearDay, uint8_t weekDay,
    uint8_t hour, uint8_t minute, bool valid = true) {
  SchedulerClock clock = {valid, year, yearDay, weekDay, hour, minute};
  return clock;
}

int main() {
  const uint32_t never = UINT32_MAX;
  SchedulerClock monday1800 = clockAt(2026, 218, 0, 18, 0);

  // 1: correct weekday and time fires.
  assert(schedulerTimeMatches(true, 1U << 0, 18, 0, monday1800, never));

  // 2: wrong weekday and wrong time do not fire.
  assert(!schedulerTimeMatches(true, 1U << 1, 18, 0, monday1800, never));
  assert(!schedulerTimeMatches(true, 1U << 0, 18, 1, monday1800, never));

  // 3: the same entry cannot execute twice in one minute.
  uint32_t mondayKey = schedulerMinuteKey(monday1800);
  assert(!schedulerTimeMatches(true, 1U << 0, 18, 0, monday1800, mondayKey));

  // 4: it can execute again on the next configured day.
  SchedulerClock tuesday1800 = clockAt(2026, 219, 1, 18, 0);
  assert(schedulerTimeMatches(true, (1U << 0) | (1U << 1), 18, 0,
    tuesday1800, mondayKey));

  // 5: disabled entries never execute.
  assert(!schedulerTimeMatches(false, 0x7F, 18, 0, monday1800, never));

  // 6 and 7: comparison results gate execution as expected.
  assert(schedulerCompare(39.5f, SCHEDULER_COMPARE_LESS, 42.0f));
  assert(!schedulerCompare(45.2f, SCHEDULER_COMPARE_LESS, 42.0f));
  assert(schedulerCompare(42.005f, SCHEDULER_COMPARE_EQUAL, 42.0f));
  assert(schedulerCompare(42.0f, SCHEDULER_COMPARE_GREATER_EQUAL, 42.0f));

  // Strict numeric parsing rejects empty, partial and non-finite payloads.
  float parsed = 0;
  assert(schedulerParseFiniteNumber("63.2", parsed) && parsed == 63.2f);
  assert(schedulerParseFiniteNumber("-250", parsed) && parsed == -250.0f);
  assert(!schedulerParseFiniteNumber("", parsed));
  assert(!schedulerParseFiniteNumber("63.2 C", parsed));
  assert(!schedulerParseFiniteNumber("nan", parsed));
  assert(!schedulerParseFiniteNumber("1e999", parsed));

  // 8: an invalid clock pauses execution.
  SchedulerClock invalidClock = clockAt(1970, 0, 3, 18, 0, false);
  assert(!schedulerTimeMatches(true, 0x7F, 18, 0, invalidClock, never));

  // 9: a boot after the configured minute does not replay history.
  SchedulerClock bootAt1820 = clockAt(2026, 218, 0, 18, 20);
  assert(!schedulerTimeMatches(true, 0x7F, 18, 0, bootAt1820, never));

  // 10: malformed weekday data is rejected by matching logic. Full JSON
  // validation is performed by SchedulerManager::parseEntry on the device.
  assert(schedulerBasicEntryValid(0x7F, 23, 59));
  assert(!schedulerBasicEntryValid(0, 18, 0));
  assert(!schedulerBasicEntryValid(0x7F, 24, 0));
  assert(!schedulerBasicEntryValid(0x7F, 18, 60));
  assert(!schedulerTimeMatches(true, 0, 18, 0, monday1800, never));
  SchedulerClock corruptWeekday = clockAt(2026, 218, 9, 18, 0);
  assert(!schedulerTimeMatches(true, 0x7F, 18, 0, corruptWeekday, never));

  // 11: multiple matching entries receive distinct bounded queue slots, while
  // dispatch remains rate-limited rather than writing both at once.
  assert(schedulerTimeMatches(true, 0x7F, 18, 0, monday1800, never));
  assert(schedulerTimeMatches(true, 0x7F, 18, 0, monday1800, never));
  uint8_t queueIndex = 255;
  assert(schedulerQueueWriteIndex(0, 0, 16, queueIndex) && queueIndex == 0);
  assert(schedulerQueueWriteIndex(0, 1, 16, queueIndex) && queueIndex == 1);
  assert(!schedulerQueueWriteIndex(0, 16, 16, queueIndex));
  assert(schedulerDispatchReady(2, 0, 1000, 2000));
  assert(!schedulerDispatchReady(1, 1000, 2999, 2000));
  assert(schedulerDispatchReady(1, 1000, 3000, 2000));

  puts("scheduler_logic_test: all checks passed");
  return 0;
}
