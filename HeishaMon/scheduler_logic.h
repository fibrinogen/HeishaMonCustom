#pragma once

#include <stdint.h>
#include <math.h>
#include <stdlib.h>

struct SchedulerClock {
  bool valid;
  uint16_t year;
  uint16_t yearDay;
  uint8_t weekDay; // Monday = 0, Sunday = 6
  uint8_t hour;
  uint8_t minute;
};

enum SchedulerCompareOperator : uint8_t {
  SCHEDULER_COMPARE_LESS = 0,
  SCHEDULER_COMPARE_LESS_EQUAL,
  SCHEDULER_COMPARE_EQUAL,
  SCHEDULER_COMPARE_GREATER_EQUAL,
  SCHEDULER_COMPARE_GREATER
};

enum SchedulerConditionJoin : uint8_t {
  SCHEDULER_JOIN_AND = 0,
  SCHEDULER_JOIN_OR,
  SCHEDULER_JOIN_COUNT
};

enum SchedulerTruthValue : uint8_t {
  SCHEDULER_TRUTH_FALSE = 0,
  SCHEDULER_TRUTH_TRUE,
  SCHEDULER_TRUTH_UNKNOWN
};

inline bool schedulerBasicEntryValid(uint8_t dayMask, uint8_t hour, uint8_t minute) {
  return dayMask > 0 && dayMask <= 0x7F && hour <= 23 && minute <= 59;
}

inline bool schedulerQueueWriteIndex(uint8_t start, uint8_t count, uint8_t capacity,
    uint8_t &index) {
  if (capacity == 0 || count >= capacity) return false;
  index = (uint8_t)((start + count) % capacity);
  return true;
}

inline bool schedulerDispatchReady(uint8_t pendingCount, unsigned long lastDispatchAt,
    unsigned long now, unsigned long interval) {
  return pendingCount > 0 &&
    (lastDispatchAt == 0 || (unsigned long)(now - lastDispatchAt) >= interval);
}

inline uint32_t schedulerMinuteKey(const SchedulerClock &clock) {
  return (((uint32_t)clock.year * 366UL + clock.yearDay) * 1440UL) +
         ((uint32_t)clock.hour * 60UL) + clock.minute;
}

inline bool schedulerDueNow(uint8_t dayMask, uint8_t hour, uint8_t minute,
    const SchedulerClock &clock, uint32_t lastExecutionKey) {
  if (!clock.valid || !schedulerBasicEntryValid(dayMask, hour, minute) ||
      clock.weekDay > 6) return false;
  if ((dayMask & (1U << clock.weekDay)) == 0) return false;
  if (hour != clock.hour || minute != clock.minute) return false;
  return lastExecutionKey != schedulerMinuteKey(clock);
}

inline bool schedulerTimeMatches(bool enabled, uint8_t dayMask, uint8_t hour,
    uint8_t minute, const SchedulerClock &clock, uint32_t lastExecutionKey) {
  return enabled && schedulerDueNow(dayMask, hour, minute, clock, lastExecutionKey);
}

inline bool schedulerCompare(float actual, SchedulerCompareOperator op, float expected) {
  switch (op) {
    case SCHEDULER_COMPARE_LESS: return actual < expected;
    case SCHEDULER_COMPARE_LESS_EQUAL: return actual <= expected;
    case SCHEDULER_COMPARE_EQUAL: return fabsf(actual - expected) < 0.01f;
    case SCHEDULER_COMPARE_GREATER_EQUAL: return actual >= expected;
    case SCHEDULER_COMPARE_GREATER: return actual > expected;
    default: return false;
  }
}

inline SchedulerTruthValue schedulerTruthAnd(SchedulerTruthValue left,
    SchedulerTruthValue right) {
  if (left == SCHEDULER_TRUTH_FALSE || right == SCHEDULER_TRUTH_FALSE) {
    return SCHEDULER_TRUTH_FALSE;
  }
  if (left == SCHEDULER_TRUTH_UNKNOWN || right == SCHEDULER_TRUTH_UNKNOWN) {
    return SCHEDULER_TRUTH_UNKNOWN;
  }
  return SCHEDULER_TRUTH_TRUE;
}

inline SchedulerTruthValue schedulerTruthOr(SchedulerTruthValue left,
    SchedulerTruthValue right) {
  if (left == SCHEDULER_TRUTH_TRUE || right == SCHEDULER_TRUTH_TRUE) {
    return SCHEDULER_TRUTH_TRUE;
  }
  if (left == SCHEDULER_TRUTH_UNKNOWN || right == SCHEDULER_TRUTH_UNKNOWN) {
    return SCHEDULER_TRUTH_UNKNOWN;
  }
  return SCHEDULER_TRUTH_FALSE;
}

inline SchedulerTruthValue schedulerEvaluateConditionExpression(
    const SchedulerTruthValue *values, const SchedulerConditionJoin *joins,
    uint8_t count) {
  if (values == nullptr || joins == nullptr || count == 0) return SCHEDULER_TRUTH_TRUE;
  SchedulerTruthValue expression = SCHEDULER_TRUTH_FALSE;
  SchedulerTruthValue andGroup = values[0];
  for (uint8_t i = 1; i < count; i++) {
    if (joins[i] == SCHEDULER_JOIN_OR) {
      expression = schedulerTruthOr(expression, andGroup);
      andGroup = values[i];
    } else {
      andGroup = schedulerTruthAnd(andGroup, values[i]);
    }
  }
  return schedulerTruthOr(expression, andGroup);
}

inline bool schedulerParseFiniteNumber(const char *text, float &value) {
  if (text == nullptr || text[0] == '\0') return false;
  char *end = nullptr;
  float parsed = strtof(text, &end);
  if (end == text || *end != '\0' || !isfinite(parsed)) return false;
  value = parsed;
  return true;
}
