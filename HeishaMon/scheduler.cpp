#include "scheduler.h"

#include <LittleFS.h>
#include <time.h>

static const char *SCHEDULER_FILE = "/scheduler.json";
static const unsigned long SCHEDULER_DISPATCH_INTERVAL_MS = 2000UL;

SchedulerManager::SchedulerManager()
  : count_(0), eventStart_(0), eventCount_(0), pendingStart_(0), pendingCount_(0),
    enabled_(true), clockWasValid_(false), clockInvalidEventRecorded_(false),
    lastCheckedMinuteKey_(UINT32_MAX), lastDispatchAt_(0), valueReader_(nullptr),
    dispatcher_(nullptr), logger_(nullptr), persistentEventLogger_(nullptr) {
  memset(entries_, 0, sizeof(entries_));
  memset(events_, 0, sizeof(events_));
  memset(pending_, 0, sizeof(pending_));
}

void SchedulerManager::begin(SchedulerValueReader valueReader,
    SchedulerActionDispatcher dispatcher, SchedulerLogger logger) {
  valueReader_ = valueReader;
  dispatcher_ = dispatcher;
  logger_ = logger;
  if (!load()) log("[SCHED] no valid scheduler configuration loaded; using safe defaults");
}

bool SchedulerManager::readClock(SchedulerClock &clock) const {
  memset(&clock, 0, sizeof(clock));
  time_t now = time(nullptr);
  struct tm local;
  if (now <= 0 || localtime_r(&now, &local) == nullptr || local.tm_year + 1900 < 2024) {
    clock.valid = false;
    return false;
  }
  clock.valid = true;
  clock.year = (uint16_t)(local.tm_year + 1900);
  clock.yearDay = (uint16_t)local.tm_yday;
  clock.weekDay = (uint8_t)((local.tm_wday + 6) % 7);
  clock.hour = (uint8_t)local.tm_hour;
  clock.minute = (uint8_t)local.tm_min;
  return true;
}

bool SchedulerManager::clockValid() const {
  SchedulerClock clock;
  return readClock(clock);
}

void SchedulerManager::loop() {
  SchedulerClock clock;
  bool valid = readClock(clock);
  if (!valid) {
    if (clockWasValid_) log("[SCHED] local time became invalid; execution paused");
    if (!clockInvalidEventRecorded_) {
      SchedulerEntry systemEntry = {};
      strlcpy(systemEntry.name, "Scheduler", sizeof(systemEntry.name));
      addEvent(systemEntry, "paused",
        "Local time unavailable; due schedules cannot be evaluated");
      clockInvalidEventRecorded_ = true;
    }
    cancelPending("Local time invalid; pending action cancelled");
    clockWasValid_ = false;
    return;
  }
  if (!clockWasValid_) log("[SCHED] valid local time available");
  if (clockInvalidEventRecorded_) {
    SchedulerEntry systemEntry = {};
    strlcpy(systemEntry.name, "Scheduler", sizeof(systemEntry.name));
    addEvent(systemEntry, "resumed",
      "Local time is valid; due schedule evaluation resumed");
  }
  clockWasValid_ = true;
  clockInvalidEventRecorded_ = false;

  uint32_t minuteKey = schedulerMinuteKey(clock);
  if (minuteKey != lastCheckedMinuteKey_) {
    lastCheckedMinuteKey_ = minuteKey;
    checkSchedules(clock);
  }
  dispatchNext();
}

void SchedulerManager::checkSchedules(const SchedulerClock &clock) {
  for (uint8_t i = 0; i < count_; i++) {
    SchedulerEntry &entry = entries_[i];
    if (!schedulerDueNow(entry.dayMask, entry.hour, entry.minute,
        clock, entry.lastExecutionKey)) continue;

    entry.lastExecutionKey = schedulerMinuteKey(clock);
    char matchMessage[96];
    snprintf(matchMessage, sizeof(matchMessage), "[SCHED] #%u %s matched at %02u:%02u",
      entry.id, entry.name, clock.hour, clock.minute);
    log(matchMessage);
    char action[96] = {0};
    describeActions(entry, action, sizeof(action));
    char decision[SCHEDULER_DETAIL_LENGTH] = {0};
    if (!enabled_) {
      snprintf(decision, sizeof(decision),
        "Due %02u:%02u; skipped: scheduler is globally paused; action: %s",
        clock.hour, clock.minute, action);
      addEvent(entry, "skipped", decision);
      continue;
    }
    if (!entry.enabled) {
      snprintf(decision, sizeof(decision),
        "Due %02u:%02u; skipped: schedule is disabled; action: %s",
        clock.hour, clock.minute, action);
      addEvent(entry, "skipped", decision);
      continue;
    }
    char detail[SCHEDULER_DETAIL_LENGTH] = {0};
    if (!evaluateCondition(entry, detail, sizeof(detail))) {
      snprintf(decision, sizeof(decision), "Due %02u:%02u; skipped: %s; action: %s",
        clock.hour, clock.minute, detail, action);
      addEvent(entry, "skipped", decision);
      continue;
    }
    snprintf(decision, sizeof(decision), "Due %02u:%02u; %s",
      clock.hour, clock.minute, detail);
    char queueMessage[64] = {0};
    if (!enqueue(entry, decision, nullptr, nullptr, nullptr,
        queueMessage, sizeof(queueMessage))) {
      snprintf(decision, sizeof(decision), "Due %02u:%02u; not queued: %s; action: %s",
        clock.hour, clock.minute, queueMessage, action);
      addEvent(entry, "failed", decision);
    }
  }
}

bool SchedulerManager::evaluateCondition(const SchedulerEntry &entry,
    char *detail, size_t detailSize) {
  if (entry.conditionCount == 0) {
    snprintf(detail, detailSize, "no conditions configured");
    return true;
  }
  if (valueReader_ == nullptr) {
    snprintf(detail, detailSize,
      "conditions unavailable: value provider is not initialized");
    return false;
  }
  SchedulerTruthValue values[SCHEDULER_MAX_CONDITIONS];
  SchedulerConditionJoin joins[SCHEDULER_MAX_CONDITIONS];
  char firstFailure[SCHEDULER_DETAIL_LENGTH] = {0};
  char firstUnavailable[SCHEDULER_DETAIL_LENGTH] = {0};
  bool hasOr = false;
  for (uint8_t i = 0; i < entry.conditionCount; i++) {
    const SchedulerCondition &condition = entry.conditions[i];
    joins[i] = i == 0 ? SCHEDULER_JOIN_AND : condition.join;
    if (i > 0 && condition.join == SCHEDULER_JOIN_OR) hasOr = true;
    float actual = 0;
    uint32_t ageSeconds = 0;
    char providerDetail[64] = {0};
    uint8_t sourceId = condition.source == SCHEDULER_SOURCE_LOCAL ?
      conditionTopic(condition.field) : condition.externalSensorId;
    if (!valueReader_(condition.source, sourceId, &actual, &ageSeconds,
        providerDetail, sizeof(providerDetail)) || !isfinite(actual)) {
      if (firstUnavailable[0] != '\0') {
        values[i] = SCHEDULER_TRUTH_UNKNOWN;
        continue;
      } else if (providerDetail[0] != '\0') {
        snprintf(firstUnavailable, sizeof(firstUnavailable),
          "condition %u/%u unavailable: %s",
          i + 1, entry.conditionCount, providerDetail);
      } else if (condition.source == SCHEDULER_SOURCE_LOCAL) {
        snprintf(firstUnavailable, sizeof(firstUnavailable),
          "condition %u/%u unavailable: %s",
          i + 1, entry.conditionCount, conditionDisplayName(condition.field));
      } else {
        snprintf(firstUnavailable, sizeof(firstUnavailable),
          "condition %u/%u unavailable: MQTT sensor %u",
          i + 1, entry.conditionCount, condition.externalSensorId);
      }
      values[i] = SCHEDULER_TRUTH_UNKNOWN;
      continue;
    }
    bool result = schedulerCompare(actual, condition.compare, condition.value);
    values[i] = result ? SCHEDULER_TRUTH_TRUE : SCHEDULER_TRUTH_FALSE;
    if (!result && firstFailure[0] == '\0') {
      if (condition.source == SCHEDULER_SOURCE_LOCAL) {
        snprintf(firstFailure, sizeof(firstFailure),
          "condition %u/%u failed: %s is %.2f, expected %s %.2f",
          i + 1, entry.conditionCount, conditionDisplayName(condition.field),
          actual, operatorName(condition.compare), condition.value);
      } else {
        snprintf(firstFailure, sizeof(firstFailure),
          "condition %u/%u failed: MQTT sensor %u is %.2f (age %lu s), expected %s %.2f",
          i + 1, entry.conditionCount, condition.externalSensorId, actual,
          (unsigned long)ageSeconds, operatorName(condition.compare), condition.value);
      }
    }
  }
  SchedulerTruthValue expression = schedulerEvaluateConditionExpression(
    values, joins, entry.conditionCount);
  if (expression == SCHEDULER_TRUTH_TRUE) {
    if (hasOr) {
      snprintf(detail, detailSize, "condition expression passed (AND before OR)");
    } else {
      snprintf(detail, detailSize, "all %u condition%s passed", entry.conditionCount,
        entry.conditionCount == 1 ? "" : "s");
    }
    return true;
  }
  if (expression == SCHEDULER_TRUTH_UNKNOWN) {
    snprintf(detail, detailSize, "expression unavailable: %s",
      firstUnavailable[0] == '\0' ? "required value unavailable" : firstUnavailable);
    return false;
  }
  snprintf(detail, detailSize, hasOr ? "all OR groups false: %s" : "%s",
    firstFailure[0] == '\0' ? "condition expression is false" : firstFailure);
  return false;
}

bool SchedulerManager::enqueue(const SchedulerEntry &entry, const char *conditionDetail,
    SchedulerDispatchGuard guard, SchedulerDispatchObserver observer, void *observerContext,
    char *message, size_t messageSize) {
  uint8_t index = 0;
  if (!schedulerQueueWriteIndex(pendingStart_, pendingCount_, SCHEDULER_MAX_ENTRIES, index)) {
    snprintf(message, messageSize, "Scheduler action queue full");
    return false;
  }
  pending_[index].entryId = entry.id;
  strlcpy(pending_[index].name, entry.name, sizeof(pending_[index].name));
  pending_[index].actionCount = entry.actionCount;
  pending_[index].nextActionIndex = 0;
  memcpy(pending_[index].actions, entry.actions,
    sizeof(SchedulerAction) * entry.actionCount);
  pending_[index].automation = entry.id == 0;
  strlcpy(pending_[index].conditionDetail, conditionDetail,
    sizeof(pending_[index].conditionDetail));
  pending_[index].guard = guard;
  pending_[index].observer = observer;
  pending_[index].observerContext = observerContext;
  pendingCount_++;
  snprintf(message, messageSize, "%u action%s queued", entry.actionCount,
    entry.actionCount == 1 ? "" : "s");
  return true;
}

void SchedulerManager::dispatchNext() {
  if (pendingCount_ == 0 || dispatcher_ == nullptr) return;
  unsigned long now = millis();
  if (!schedulerDispatchReady(pendingCount_, lastDispatchAt_, now,
      SCHEDULER_DISPATCH_INTERVAL_MS)) return;

  PendingActionGroup &pending = pending_[pendingStart_];
  uint8_t actionIndex = pending.nextActionIndex;
  SchedulerAction action = pending.actions[actionIndex];
  SchedulerDispatchObserver observer = pending.observer;
  void *observerContext = pending.observerContext;
  lastDispatchAt_ = now;

  SchedulerEntry eventEntry = {};
  eventEntry.id = pending.entryId;
  eventEntry.actionCount = 1;
  eventEntry.actions[0] = action;
  strlcpy(eventEntry.name, pending.name, sizeof(eventEntry.name));

  char actionDetail[SCHEDULER_DETAIL_LENGTH] = {0};
  SchedulerDispatchResult result;
  if (!pending.automation && !enabled_) {
    snprintf(actionDetail, sizeof(actionDetail), "Scheduler disabled before dispatch");
    result = SCHEDULER_DISPATCH_FAILED;
  } else if (pending.guard != nullptr && !pending.guard(pending.observerContext)) {
    snprintf(actionDetail, sizeof(actionDetail), "Automation request cancelled before dispatch");
    result = SCHEDULER_DISPATCH_FAILED;
  } else {
    result = dispatcher_(action.type, action.value, actionDetail, sizeof(actionDetail));
  }
  char actionDescription[64] = {0};
  describeAction(action, actionDescription, sizeof(actionDescription));
  char detail[SCHEDULER_DETAIL_LENGTH] = {0};
  snprintf(detail, sizeof(detail), "%s; action %u/%u %s: %s",
    pending.conditionDetail, actionIndex + 1, pending.actionCount,
    actionDescription, actionDetail);
  switch (result) {
    case SCHEDULER_DISPATCH_EXECUTED: addEvent(eventEntry, "executed", detail); break;
    case SCHEDULER_DISPATCH_NO_CHANGE: addEvent(eventEntry, "no change", detail); break;
    case SCHEDULER_DISPATCH_BUSY: addEvent(eventEntry, "busy", detail); break;
    default: addEvent(eventEntry, "failed", detail); break;
  }
  bool continueGroup = schedulerSequenceContinues(result);
  if (continueGroup && actionIndex + 1 < pending.actionCount) {
    pending.nextActionIndex++;
  } else {
    if (!continueGroup && actionIndex + 1 < pending.actionCount) {
      char cancellation[SCHEDULER_DETAIL_LENGTH] = {0};
      snprintf(cancellation, sizeof(cancellation),
        "Action %u/%u stopped sequence; %u remaining action%s cancelled",
        actionIndex + 1, pending.actionCount, pending.actionCount - actionIndex - 1,
        pending.actionCount - actionIndex - 1 == 1 ? "" : "s");
      addEvent(eventEntry, "cancelled", cancellation);
    }
    pendingStart_ = (uint8_t)((pendingStart_ + 1) % SCHEDULER_MAX_ENTRIES);
    pendingCount_--;
  }
  if (observer != nullptr) {
    observer(result, actionDetail, observerContext);
  }
}

void SchedulerManager::cancelPending(const char *reason) {
  while (pendingCount_ > 0) {
    PendingActionGroup pending = pending_[pendingStart_];
    pendingStart_ = (uint8_t)((pendingStart_ + 1) % SCHEDULER_MAX_ENTRIES);
    pendingCount_--;
    SchedulerEntry eventEntry = {};
    eventEntry.id = pending.entryId;
    eventEntry.actionCount = 1;
    eventEntry.actions[0] = pending.actions[pending.nextActionIndex];
    strlcpy(eventEntry.name, pending.name, sizeof(eventEntry.name));
    addEvent(eventEntry, "failed", reason);
    if (pending.observer != nullptr) {
      pending.observer(SCHEDULER_DISPATCH_FAILED, reason, pending.observerContext);
    }
  }
  pendingStart_ = 0;
}

void SchedulerManager::addEvent(const SchedulerEntry &entry, const char *result, const char *detail) {
  uint8_t index;
  if (eventCount_ < SCHEDULER_MAX_EVENTS) {
    index = (uint8_t)((eventStart_ + eventCount_) % SCHEDULER_MAX_EVENTS);
    eventCount_++;
  } else {
    index = eventStart_;
    eventStart_ = (uint8_t)((eventStart_ + 1) % SCHEDULER_MAX_EVENTS);
  }

  SchedulerEvent &event = events_[index];
  time_t now = time(nullptr);
  struct tm local;
  if (now > 0 && localtime_r(&now, &local) != nullptr) {
    strftime(event.timestamp, sizeof(event.timestamp), "%Y-%m-%d %H:%M:%S", &local);
  } else {
    strlcpy(event.timestamp, "time unavailable", sizeof(event.timestamp));
  }
  event.entryId = entry.id;
  strlcpy(event.name, entry.name, sizeof(event.name));
  strlcpy(event.result, result, sizeof(event.result));
  strlcpy(event.detail, detail, sizeof(event.detail));

  char message[256];
  snprintf(message, sizeof(message), "[SCHED] #%u %s -> %s: %s",
    entry.id, entry.name, result, detail);
  log(message);
  if (persistentEventLogger_ != nullptr) persistentEventLogger_(&event);
}

void SchedulerManager::log(const char *message) const {
  if (logger_ != nullptr) logger_((char *)message);
}

int8_t SchedulerManager::findIndex(uint8_t id) const {
  for (uint8_t i = 0; i < count_; i++) if (entries_[i].id == id) return (int8_t)i;
  return -1;
}

uint8_t SchedulerManager::nextId() const {
  for (uint16_t id = 1; id <= 255; id++) if (findIndex((uint8_t)id) < 0) return (uint8_t)id;
  return 0;
}

uint8_t SchedulerManager::enabledCount() const {
  uint8_t result = 0;
  for (uint8_t i = 0; i < count_; i++) if (entries_[i].enabled) result++;
  return result;
}

uint8_t SchedulerManager::pendingActionCount() const {
  uint8_t result = 0;
  for (uint8_t offset = 0; offset < pendingCount_; offset++) {
    uint8_t index = (uint8_t)((pendingStart_ + offset) % SCHEDULER_MAX_ENTRIES);
    const PendingActionGroup &pending = pending_[index];
    result = (uint8_t)(result + pending.actionCount - pending.nextActionIndex);
  }
  return result;
}

bool SchedulerManager::validateAction(const SchedulerAction &action,
    char *message, size_t messageSize) {
  if (action.type >= SCHEDULER_ACTION_COUNT) {
    snprintf(message, messageSize, "Unknown action"); return false;
  }
  switch (action.type) {
    case SCHEDULER_ACTION_SET_OPERATION_MODE:
      if (action.value < 0 || action.value > 6) { snprintf(message, messageSize, "Operating mode must be 0..6"); return false; }
      break;
    case SCHEDULER_ACTION_SET_DHW_TARGET:
      if (action.value < 40 || action.value > 75) { snprintf(message, messageSize, "DHW target must be 40..75 C"); return false; }
      break;
    case SCHEDULER_ACTION_SET_HEAT_CURVE_SHIFT:
      if (action.value < -5 || action.value > 5) { snprintf(message, messageSize, "Heating curve shift must be -5..5 K"); return false; }
      break;
    case SCHEDULER_ACTION_SET_Z1_HEATING_WATER_TARGET:
      if (action.value < 20 || action.value > 100) { snprintf(message, messageSize, "Heating water target must be 20..100 C"); return false; }
      break;
    case SCHEDULER_ACTION_SET_Z1_ROOM_TARGET:
      if (action.value < 10 || action.value > 35) { snprintf(message, messageSize, "Room target must be 10..35 C"); return false; }
      break;
    case SCHEDULER_ACTION_SET_Z1_REQUEST:
      break;
    case SCHEDULER_ACTION_SET_QUIET_MODE:
      if (action.value < 0 || action.value > 3) { snprintf(message, messageSize, "Quiet mode must be 0..3"); return false; }
      break;
    default: break;
  }
  return true;
}

bool SchedulerManager::validateEntry(const SchedulerEntry &entry,
    char *message, size_t messageSize) const {
  size_t nameLength = strnlen(entry.name, sizeof(entry.name));
  if (nameLength == 0 || nameLength >= sizeof(entry.name)) {
    snprintf(message, messageSize, "Name is required and must be at most 32 characters"); return false;
  }
  if (entry.dayMask == 0 || entry.dayMask > 0x7F) {
    snprintf(message, messageSize, "At least one valid weekday is required"); return false;
  }
  if (!schedulerBasicEntryValid(entry.dayMask, entry.hour, entry.minute)) {
    snprintf(message, messageSize, "Time is outside 00:00..23:59"); return false;
  }
  if (entry.actionCount == 0 || entry.actionCount > SCHEDULER_MAX_ACTIONS) {
    snprintf(message, messageSize, "One to %u actions are required", SCHEDULER_MAX_ACTIONS);
    return false;
  }
  for (uint8_t i = 0; i < entry.actionCount; i++) {
    char actionMessage[80] = {0};
    if (!validateAction(entry.actions[i], actionMessage, sizeof(actionMessage))) {
      snprintf(message, messageSize, "Action %u: %s", i + 1, actionMessage);
      return false;
    }
    if (entry.actions[i].type == SCHEDULER_ACTION_FORCE_DHW &&
        i + 1 < entry.actionCount) {
      snprintf(message, messageSize, "Force DHW must be the final action");
      return false;
    }
  }
  if (entry.conditionCount > SCHEDULER_MAX_CONDITIONS) {
    snprintf(message, messageSize, "At most %u conditions are supported", SCHEDULER_MAX_CONDITIONS);
    return false;
  }
  for (uint8_t i = 0; i < entry.conditionCount; i++) {
    const SchedulerCondition &condition = entry.conditions[i];
    if (condition.source >= SCHEDULER_SOURCE_COUNT ||
        condition.join >= SCHEDULER_JOIN_COUNT ||
        condition.compare > SCHEDULER_COMPARE_GREATER || !isfinite(condition.value) ||
        condition.value < -100.0f || condition.value > 200.0f) {
      snprintf(message, messageSize, "Invalid condition"); return false;
    }
    if (condition.source == SCHEDULER_SOURCE_LOCAL) {
      if (condition.field == SCHEDULER_CONDITION_NONE ||
          condition.field >= SCHEDULER_CONDITION_COUNT) {
        snprintf(message, messageSize, "Invalid local condition"); return false;
      }
    } else if (condition.externalSensorId == 0) {
      snprintf(message, messageSize, "Invalid external sensor ID"); return false;
    }
  }
  snprintf(message, messageSize, "OK");
  return true;
}

bool SchedulerManager::parseEntry(JsonObjectConst object, SchedulerEntry &entry,
    char *message, size_t messageSize) const {
  memset(&entry, 0, sizeof(entry));
  long id = object["id"] | 0L;
  long days = object["days"] | -1L;
  long hour = object["hour"] | -1L;
  long minute = object["minute"] | -1L;
  long actionValue = object["actionValue"] | 0L;
  if (id < 0 || id > 255 || days < 0 || days > 0x7F ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      actionValue < INT16_MIN || actionValue > INT16_MAX) {
    snprintf(message, messageSize, "Numeric scheduler field outside supported range");
    return false;
  }
  const char *name = object["name"] | "";
  if (strlen(name) > SCHEDULER_NAME_LENGTH - 1) {
    snprintf(message, messageSize, "Name must be at most 32 characters");
    return false;
  }
  entry.id = (uint8_t)id;
  entry.enabled = object["enabled"] | true;
  strlcpy(entry.name, name, sizeof(entry.name));
  entry.dayMask = (uint8_t)days;
  entry.hour = (uint8_t)hour;
  entry.minute = (uint8_t)minute;
  entry.lastExecutionKey = UINT32_MAX;

  JsonArrayConst actions = object["actions"].as<JsonArrayConst>();
  if (!actions.isNull()) {
    for (JsonObjectConst actionObject : actions) {
      if (entry.actionCount >= SCHEDULER_MAX_ACTIONS) {
        snprintf(message, messageSize, "At most %u actions are supported", SCHEDULER_MAX_ACTIONS);
        return false;
      }
      SchedulerAction &action = entry.actions[entry.actionCount];
      if (!parseAction(actionObject["action"] | "", action.type)) {
        snprintf(message, messageSize, "Unknown action"); return false;
      }
      long value = actionObject["value"] | 0L;
      if (value < INT16_MIN || value > INT16_MAX) {
        snprintf(message, messageSize, "Action value outside supported range");
        return false;
      }
      action.value = (int16_t)value;
      entry.actionCount++;
    }
  } else {
    entry.actionCount = 1;
    if (!parseAction(object["action"] | "", entry.actions[0].type)) {
      snprintf(message, messageSize, "Unknown action"); return false;
    }
    entry.actions[0].value = (int16_t)actionValue;
  }
  JsonArrayConst conditions = object["conditions"].as<JsonArrayConst>();
  if (!conditions.isNull()) {
    for (JsonObjectConst conditionObject : conditions) {
      if (entry.conditionCount >= SCHEDULER_MAX_CONDITIONS) {
        snprintf(message, messageSize, "At most %u conditions are supported", SCHEDULER_MAX_CONDITIONS);
        return false;
      }
      SchedulerCondition &condition = entry.conditions[entry.conditionCount];
      if (!parseJoin(conditionObject["join"] | "and", condition.join)) {
        snprintf(message, messageSize, "Unknown condition join"); return false;
      }
      if (!parseSource(conditionObject["source"] | "local", condition.source)) {
        snprintf(message, messageSize, "Unknown condition source"); return false;
      }
      if (!parseOperator(conditionObject["operator"] | "<", condition.compare)) {
        snprintf(message, messageSize, "Unknown condition operator"); return false;
      }
      condition.value = conditionObject["value"] | 0.0f;
      if (condition.source == SCHEDULER_SOURCE_LOCAL) {
        if (!parseCondition(conditionObject["field"] | "none", condition.field)) {
          snprintf(message, messageSize, "Unknown condition field"); return false;
        }
        condition.externalSensorId = 0;
      } else {
        condition.field = SCHEDULER_CONDITION_NONE;
        long sensorId = conditionObject["sensorId"] | 0L;
        if (sensorId < 1 || sensorId > 255) {
          snprintf(message, messageSize, "Invalid external sensor ID"); return false;
        }
        condition.externalSensorId = (uint8_t)sensorId;
      }
      entry.conditionCount++;
    }
  } else {
    SchedulerConditionField field;
    SchedulerCompareOperator compare;
    if (!parseCondition(object["conditionField"] | "none", field) ||
        !parseOperator(object["conditionOperator"] | "<", compare)) {
      snprintf(message, messageSize, "Unknown legacy condition"); return false;
    }
    if (field != SCHEDULER_CONDITION_NONE) {
      entry.conditionCount = 1;
      entry.conditions[0].join = SCHEDULER_JOIN_AND;
      entry.conditions[0].source = SCHEDULER_SOURCE_LOCAL;
      entry.conditions[0].field = field;
      entry.conditions[0].externalSensorId = 0;
      entry.conditions[0].compare = compare;
      entry.conditions[0].value = object["conditionValue"] | 0.0f;
    }
  }
  return validateEntry(entry, message, messageSize);
}

bool SchedulerManager::upsert(JsonObjectConst object, char *message, size_t messageSize) {
  SchedulerEntry entry;
  if (!parseEntry(object, entry, message, messageSize)) return false;
  int8_t index = entry.id == 0 ? -1 : findIndex(entry.id);
  if (entry.id != 0 && index < 0) {
    snprintf(message, messageSize, "Schedule ID not found"); return false;
  }
  bool adding = index < 0;
  SchedulerEntry previous = {};
  if (adding) {
    if (count_ >= SCHEDULER_MAX_ENTRIES) {
      snprintf(message, messageSize, "Maximum of %u schedules reached", SCHEDULER_MAX_ENTRIES);
      return false;
    }
    entry.id = nextId();
    if (entry.id == 0) { snprintf(message, messageSize, "No schedule ID available"); return false; }
    entries_[count_++] = entry;
  } else {
    previous = entries_[index];
    entry.lastExecutionKey = entries_[index].lastExecutionKey;
    entries_[index] = entry;
  }
  if (!save()) {
    if (adding) count_--;
    else entries_[index] = previous;
    snprintf(message, messageSize, "Could not persist schedule");
    return false;
  }
  snprintf(message, messageSize, "Schedule saved");
  return true;
}

bool SchedulerManager::remove(uint8_t id, char *message, size_t messageSize) {
  int8_t index = findIndex(id);
  if (index < 0) { snprintf(message, messageSize, "Schedule not found"); return false; }
  SchedulerEntry removed = entries_[index];
  for (uint8_t i = (uint8_t)index; i + 1 < count_; i++) entries_[i] = entries_[i + 1];
  count_--;
  if (!save()) {
    for (uint8_t i = count_; i > (uint8_t)index; i--) entries_[i] = entries_[i - 1];
    entries_[index] = removed;
    count_++;
    snprintf(message, messageSize, "Could not persist removal");
    return false;
  }
  snprintf(message, messageSize, "Schedule removed");
  return true;
}

bool SchedulerManager::runNow(uint8_t id, char *message, size_t messageSize) {
  if (!enabled_) {
    snprintf(message, messageSize, "Scheduler is disabled");
    return false;
  }
  int8_t index = findIndex(id);
  if (index < 0) { snprintf(message, messageSize, "Schedule not found"); return false; }
  char action[96] = {0};
  describeActions(entries_[index], action, sizeof(action));
  char detail[SCHEDULER_DETAIL_LENGTH];
  if (!evaluateCondition(entries_[index], detail, sizeof(detail))) {
    char decision[SCHEDULER_DETAIL_LENGTH];
    snprintf(decision, sizeof(decision), "Manual run skipped: %s; action: %s",
      detail, action);
    addEvent(entries_[index], "skipped", decision);
    snprintf(message, messageSize, "Action skipped: %s", detail);
    return true;
  }
  char decision[SCHEDULER_DETAIL_LENGTH];
  snprintf(decision, sizeof(decision), "Manual run; %s", detail);
  return enqueue(entries_[index], decision, nullptr, nullptr, nullptr,
    message, messageSize);
}

bool SchedulerManager::claimDue(bool enabled, uint8_t dayMask, uint8_t hour,
    uint8_t minute, uint32_t &lastExecutionKey) const {
  SchedulerClock clock;
  if (!readClock(clock) ||
      !schedulerTimeMatches(enabled, dayMask, hour, minute, clock, lastExecutionKey)) {
    return false;
  }
  lastExecutionKey = schedulerMinuteKey(clock);
  return true;
}

bool SchedulerManager::submitAutomationAction(const char *name, SchedulerActionType action,
    int16_t value, const char *reason, SchedulerDispatchGuard guard,
    SchedulerDispatchObserver observer, void *context,
    char *message, size_t messageSize) {
  if (action >= SCHEDULER_ACTION_COUNT || name == nullptr || name[0] == '\0') {
    snprintf(message, messageSize, "Invalid automation action");
    return false;
  }
  SchedulerEntry entry = {};
  entry.id = 0;
  entry.actionCount = 1;
  entry.actions[0].type = action;
  entry.actions[0].value = value;
  strlcpy(entry.name, name, sizeof(entry.name));
  return enqueue(entry, reason == nullptr ? "Automation request" : reason,
    guard, observer, context, message, messageSize);
}

bool SchedulerManager::setEnabled(bool enabled, char *message, size_t messageSize) {
  bool previous = enabled_;
  enabled_ = enabled;
  if (!save()) {
    enabled_ = previous;
    snprintf(message, messageSize, "Could not persist scheduler state");
    return false;
  }
  snprintf(message, messageSize, enabled_ ? "Scheduler enabled" : "Scheduler disabled");
  return true;
}

bool SchedulerManager::load() {
  count_ = 0;
  pendingStart_ = 0;
  pendingCount_ = 0;
  enabled_ = true;
  if (!LittleFS.begin() || !LittleFS.exists(SCHEDULER_FILE)) return true;
  File file = LittleFS.open(SCHEDULER_FILE, "r");
  if (!file) return false;
  JsonDocument document;
  DeserializationError error = deserializeJson(document, file);
  file.close();
  int version = document["version"] | 0;
  if (error || version < 1 || version > SCHEDULER_CONFIG_VERSION) {
    enabled_ = false;
    return false;
  }
  enabled_ = document["enabled"] | true;
  JsonArrayConst entries = document["entries"].as<JsonArrayConst>();
  for (JsonObjectConst object : entries) {
    if (count_ >= SCHEDULER_MAX_ENTRIES) break;
    SchedulerEntry entry;
    char message[96];
    if (!parseEntry(object, entry, message, sizeof(message)) || entry.id == 0 || findIndex(entry.id) >= 0) {
      char logMessage[160];
      snprintf(logMessage, sizeof(logMessage), "[SCHED] ignored invalid stored entry: %s", message);
      log(logMessage);
      continue;
    }
    for (uint8_t actionIndex = 0; actionIndex < entry.actionCount; actionIndex++) {
      if (entry.actions[actionIndex].type == SCHEDULER_ACTION_SET_Z1_REQUEST) {
        entry.enabled = false;
        log("[SCHED] legacy set_z1_request disabled; edit it to choose a semantic Zone 1 action");
        break;
      }
    }
    entries_[count_++] = entry;
  }
  return true;
}

bool SchedulerManager::save() {
  if (!LittleFS.begin()) return false;
  JsonDocument document;
  document["version"] = SCHEDULER_CONFIG_VERSION;
  document["enabled"] = enabled_;
  JsonArray entries = document["entries"].to<JsonArray>();
  for (uint8_t i = 0; i < count_; i++) entryToJson(entries_[i], entries.add<JsonObject>());
  File file = LittleFS.open(SCHEDULER_FILE, "w");
  if (!file) return false;
  bool ok = serializeJson(document, file) > 0;
  file.close();
  return ok;
}

void SchedulerManager::entryToJson(const SchedulerEntry &entry, JsonObject object) const {
  object["id"] = entry.id;
  object["enabled"] = entry.enabled;
  object["name"] = entry.name;
  object["days"] = entry.dayMask;
  object["hour"] = entry.hour;
  object["minute"] = entry.minute;
  object["action"] = actionName(entry.actions[0].type);
  object["actionValue"] = entry.actions[0].value;
  JsonArray actions = object["actions"].to<JsonArray>();
  for (uint8_t i = 0; i < entry.actionCount; i++) {
    JsonObject actionObject = actions.add<JsonObject>();
    actionObject["action"] = actionName(entry.actions[i].type);
    actionObject["value"] = entry.actions[i].value;
  }
  JsonArray conditions = object["conditions"].to<JsonArray>();
  for (uint8_t i = 0; i < entry.conditionCount; i++) {
    const SchedulerCondition &condition = entry.conditions[i];
    JsonObject conditionObject = conditions.add<JsonObject>();
    conditionObject["join"] = joinName(i == 0 ? SCHEDULER_JOIN_AND : condition.join);
    conditionObject["source"] = sourceName(condition.source);
    conditionObject["operator"] = operatorName(condition.compare);
    conditionObject["value"] = condition.value;
    if (condition.source == SCHEDULER_SOURCE_LOCAL) {
      conditionObject["field"] = conditionName(condition.field);
    } else {
      conditionObject["sensorId"] = condition.externalSensorId;
    }
  }
  if (entry.conditionCount > 0 && entry.conditions[0].source == SCHEDULER_SOURCE_LOCAL) {
    object["conditionField"] = conditionName(entry.conditions[0].field);
    object["conditionOperator"] = operatorName(entry.conditions[0].compare);
    object["conditionValue"] = entry.conditions[0].value;
  } else {
    object["conditionField"] = "none";
    object["conditionOperator"] = "<";
    object["conditionValue"] = 0;
  }
}

void SchedulerManager::toJson(JsonDocument &document) const {
  document["version"] = SCHEDULER_CONFIG_VERSION;
  document["enabled"] = enabled_;
  document["count"] = count_;
  document["enabledCount"] = enabledCount();
  document["maxEntries"] = SCHEDULER_MAX_ENTRIES;
  document["timeValid"] = clockValid();

  char localTime[20] = "Time unavailable";
  time_t now = time(nullptr);
  struct tm local;
  if (now > 0 && localtime_r(&now, &local) != nullptr && local.tm_year + 1900 >= 2024) {
    strftime(localTime, sizeof(localTime), "%Y-%m-%d %H:%M:%S", &local);
  }
  document["localTime"] = localTime;
  document["pendingActions"] = pendingActionCount();
  float mainScheduleState = 0;
  uint32_t mainScheduleAge = 0;
  char mainScheduleDetail[64] = {0};
  bool mainScheduleKnown = valueReader_ != nullptr && valueReader_(SCHEDULER_SOURCE_LOCAL,
    13, &mainScheduleState, &mainScheduleAge, mainScheduleDetail,
    sizeof(mainScheduleDetail));
  document["panasonicSchedulerKnown"] = mainScheduleKnown;
  if (mainScheduleKnown) document["panasonicSchedulerEnabled"] = lroundf(mainScheduleState) != 0;
  else document["panasonicSchedulerEnabled"] = nullptr;
  document["localSchedulerIndependent"] = true;

  char nextAction[96] = "None configured";
  if (clockValid()) {
    SchedulerClock current;
    readClock(current);
    uint16_t currentMinutes = (uint16_t)current.hour * 60U + current.minute;
    uint8_t bestDayOffset = 8;
    uint16_t bestMinutes = 1440;
    const SchedulerEntry *bestEntry = nullptr;
    for (uint8_t dayOffset = 0; dayOffset <= 7; dayOffset++) {
      uint8_t weekDay = (uint8_t)((current.weekDay + dayOffset) % 7);
      for (uint8_t i = 0; i < count_; i++) {
        const SchedulerEntry &entry = entries_[i];
        if (!entry.enabled || (entry.dayMask & (1U << weekDay)) == 0) continue;
        uint16_t entryMinutes = (uint16_t)entry.hour * 60U + entry.minute;
        if (dayOffset == 0 && entryMinutes <= currentMinutes) continue;
        if (dayOffset < bestDayOffset ||
            (dayOffset == bestDayOffset && entryMinutes < bestMinutes)) {
          bestDayOffset = dayOffset;
          bestMinutes = entryMinutes;
          bestEntry = &entry;
        }
      }
    }
    if (bestEntry != nullptr) snprintf(nextAction, sizeof(nextAction),
      "%s at %02u:%02u (%u day%s)", bestEntry->name, bestEntry->hour,
      bestEntry->minute, bestDayOffset, bestDayOffset == 1 ? "" : "s");
  }
  document["nextScheduledAction"] = nextAction;

  JsonArray entries = document["entries"].to<JsonArray>();
  for (uint8_t i = 0; i < count_; i++) entryToJson(entries_[i], entries.add<JsonObject>());

  JsonArray events = document["events"].to<JsonArray>();
  for (uint8_t offset = 0; offset < eventCount_; offset++) {
    uint8_t index = (uint8_t)((eventStart_ + eventCount_ - 1 - offset) % SCHEDULER_MAX_EVENTS);
    const SchedulerEvent &event = events_[index];
    JsonObject object = events.add<JsonObject>();
    object["time"] = event.timestamp;
    object["id"] = event.entryId;
    object["name"] = event.name;
    object["result"] = event.result;
    object["detail"] = event.detail;
  }
}

const char *SchedulerManager::actionName(SchedulerActionType action) {
  static const char *names[] = {"force_dhw", "heatpump_on", "heatpump_off",
    "set_operation_mode", "set_dhw_target", "set_heat_curve_shift",
    "set_z1_heating_water_target", "set_z1_room_target", "set_z1_request",
    "set_quiet_mode"};
  return action < SCHEDULER_ACTION_COUNT ? names[action] : "unknown";
}

const char *SchedulerManager::conditionName(SchedulerConditionField field) {
  static const char *names[] = {"none", "dhw_temperature", "outside_temperature",
    "room_temperature", "main_inlet_temperature", "main_outlet_temperature",
    "three_way_valve", "force_dhw"};
  return field < SCHEDULER_CONDITION_COUNT ? names[field] : "unknown";
}

const char *SchedulerManager::conditionDisplayName(SchedulerConditionField field) {
  static const char *names[] = {"condition", "DHW temperature", "outside temperature",
    "Zone 1 room temperature", "main inlet temperature", "main outlet temperature",
    "three-way valve state", "Force DHW state"};
  return field < SCHEDULER_CONDITION_COUNT ? names[field] : "unknown condition";
}

void SchedulerManager::describeAction(const SchedulerAction &action,
    char *description, size_t descriptionSize) {
  switch (action.type) {
    case SCHEDULER_ACTION_FORCE_DHW:
      snprintf(description, descriptionSize, "Force DHW workflow"); break;
    case SCHEDULER_ACTION_HEATPUMP_ON:
      snprintf(description, descriptionSize, "Heat pump on"); break;
    case SCHEDULER_ACTION_HEATPUMP_OFF:
      snprintf(description, descriptionSize, "Heat pump off"); break;
    case SCHEDULER_ACTION_SET_OPERATION_MODE:
      snprintf(description, descriptionSize, "Set operating mode to %d", action.value); break;
    case SCHEDULER_ACTION_SET_DHW_TARGET:
      snprintf(description, descriptionSize, "Set DHW target to %d C", action.value); break;
    case SCHEDULER_ACTION_SET_HEAT_CURVE_SHIFT:
      snprintf(description, descriptionSize, "Set heating curve shift to %d K", action.value); break;
    case SCHEDULER_ACTION_SET_Z1_HEATING_WATER_TARGET:
      snprintf(description, descriptionSize, "Set heating water target to %d C", action.value); break;
    case SCHEDULER_ACTION_SET_Z1_ROOM_TARGET:
      snprintf(description, descriptionSize, "Set room target to %d C", action.value); break;
    case SCHEDULER_ACTION_SET_Z1_REQUEST:
      snprintf(description, descriptionSize, "Legacy Zone 1 request %d", action.value); break;
    case SCHEDULER_ACTION_SET_QUIET_MODE:
      snprintf(description, descriptionSize, "Set quiet mode to %d", action.value); break;
    default:
      snprintf(description, descriptionSize, "Unknown action"); break;
  }
}

void SchedulerManager::describeActions(const SchedulerEntry &entry,
    char *description, size_t descriptionSize) {
  if (descriptionSize == 0) return;
  description[0] = '\0';
  for (uint8_t i = 0; i < entry.actionCount; i++) {
    char action[64] = {0};
    describeAction(entry.actions[i], action, sizeof(action));
    size_t used = strlen(description);
    if (used >= descriptionSize - 1) break;
    snprintf(description + used, descriptionSize - used, "%s%s",
      i == 0 ? "" : " -> ", action);
  }
}

const char *SchedulerManager::sourceName(SchedulerConditionSource source) {
  return source == SCHEDULER_SOURCE_MQTT ? "mqtt" : "local";
}

const char *SchedulerManager::joinName(SchedulerConditionJoin join) {
  return join == SCHEDULER_JOIN_OR ? "or" : "and";
}

const char *SchedulerManager::operatorName(SchedulerCompareOperator op) {
  static const char *names[] = {"<", "<=", "==", ">=", ">"};
  return op <= SCHEDULER_COMPARE_GREATER ? names[op] : "?";
}

bool SchedulerManager::parseAction(const char *name, SchedulerActionType &action) {
  for (uint8_t i = 0; i < SCHEDULER_ACTION_COUNT; i++) {
    if (strcmp(name, actionName((SchedulerActionType)i)) == 0) { action = (SchedulerActionType)i; return true; }
  }
  return false;
}

bool SchedulerManager::parseCondition(const char *name, SchedulerConditionField &field) {
  for (uint8_t i = 0; i < SCHEDULER_CONDITION_COUNT; i++) {
    if (strcmp(name, conditionName((SchedulerConditionField)i)) == 0) { field = (SchedulerConditionField)i; return true; }
  }
  return false;
}

bool SchedulerManager::parseSource(const char *name, SchedulerConditionSource &source) {
  if (strcmp(name, "local") == 0) {
    source = SCHEDULER_SOURCE_LOCAL;
    return true;
  }
  if (strcmp(name, "mqtt") == 0 || strcmp(name, "external_mqtt") == 0) {
    source = SCHEDULER_SOURCE_MQTT;
    return true;
  }
  return false;
}

bool SchedulerManager::parseOperator(const char *name, SchedulerCompareOperator &op) {
  for (uint8_t i = 0; i <= SCHEDULER_COMPARE_GREATER; i++) {
    if (strcmp(name, operatorName((SchedulerCompareOperator)i)) == 0) { op = (SchedulerCompareOperator)i; return true; }
  }
  return false;
}

bool SchedulerManager::parseJoin(const char *name, SchedulerConditionJoin &join) {
  if (strcmp(name, "and") == 0) { join = SCHEDULER_JOIN_AND; return true; }
  if (strcmp(name, "or") == 0) { join = SCHEDULER_JOIN_OR; return true; }
  return false;
}

uint8_t SchedulerManager::conditionTopic(SchedulerConditionField field) {
  switch (field) {
    case SCHEDULER_CONDITION_DHW_TEMPERATURE: return 10;
    case SCHEDULER_CONDITION_OUTSIDE_TEMPERATURE: return 14;
    case SCHEDULER_CONDITION_ROOM_TEMPERATURE: return 56;
    case SCHEDULER_CONDITION_MAIN_INLET_TEMPERATURE: return 5;
    case SCHEDULER_CONDITION_MAIN_OUTLET_TEMPERATURE: return 6;
    case SCHEDULER_CONDITION_THREE_WAY_VALVE: return 20;
    case SCHEDULER_CONDITION_FORCE_DHW: return 2;
    default: return 255;
  }
}
