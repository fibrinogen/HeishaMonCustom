#include "scheduler.h"

#include <LittleFS.h>
#include <time.h>

static const char *SCHEDULER_FILE = "/scheduler.json";
static const unsigned long SCHEDULER_DISPATCH_INTERVAL_MS = 2000UL;

SchedulerManager::SchedulerManager()
  : count_(0), eventStart_(0), eventCount_(0), pendingStart_(0), pendingCount_(0),
    enabled_(true), clockWasValid_(false), lastCheckedMinuteKey_(UINT32_MAX),
    lastDispatchAt_(0), valueReader_(nullptr), dispatcher_(nullptr), logger_(nullptr) {
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
    cancelPending("Local time invalid; pending action cancelled");
    clockWasValid_ = false;
    return;
  }
  if (!clockWasValid_) log("[SCHED] valid local time available");
  clockWasValid_ = true;

  uint32_t minuteKey = schedulerMinuteKey(clock);
  if (enabled_ && minuteKey != lastCheckedMinuteKey_) {
    lastCheckedMinuteKey_ = minuteKey;
    checkSchedules(clock);
  }
  dispatchNext();
}

void SchedulerManager::checkSchedules(const SchedulerClock &clock) {
  for (uint8_t i = 0; i < count_; i++) {
    SchedulerEntry &entry = entries_[i];
    if (!schedulerTimeMatches(entry.enabled, entry.dayMask, entry.hour, entry.minute,
        clock, entry.lastExecutionKey)) continue;

    entry.lastExecutionKey = schedulerMinuteKey(clock);
    char matchMessage[96];
    snprintf(matchMessage, sizeof(matchMessage), "[SCHED] #%u %s matched at %02u:%02u",
      entry.id, entry.name, clock.hour, clock.minute);
    log(matchMessage);
    char detail[SCHEDULER_DETAIL_LENGTH] = {0};
    if (!evaluateCondition(entry, detail, sizeof(detail))) {
      addEvent(entry, "skipped", detail);
      continue;
    }
    char queueMessage[64] = {0};
    if (!enqueue(entry, detail, nullptr, nullptr, nullptr,
        queueMessage, sizeof(queueMessage))) {
      addEvent(entry, "failed", queueMessage);
    }
  }
}

bool SchedulerManager::evaluateCondition(const SchedulerEntry &entry,
    char *detail, size_t detailSize) {
  if (entry.conditionCount == 0) {
    snprintf(detail, detailSize, "No condition");
    return true;
  }
  if (valueReader_ == nullptr) {
    snprintf(detail, detailSize, "Condition value provider unavailable");
    return false;
  }
  size_t used = 0;
  for (uint8_t i = 0; i < entry.conditionCount; i++) {
    const SchedulerCondition &condition = entry.conditions[i];
    float actual = 0;
    uint32_t ageSeconds = 0;
    char providerDetail[64] = {0};
    uint8_t sourceId = condition.source == SCHEDULER_SOURCE_LOCAL ?
      conditionTopic(condition.field) : condition.externalSensorId;
    if (!valueReader_(condition.source, sourceId, &actual, &ageSeconds,
        providerDetail, sizeof(providerDetail)) || !isfinite(actual)) {
      if (providerDetail[0] != '\0') {
        snprintf(detail, detailSize, "%s", providerDetail);
      } else if (condition.source == SCHEDULER_SOURCE_LOCAL) {
        snprintf(detail, detailSize, "%s unavailable", conditionName(condition.field));
      } else {
        snprintf(detail, detailSize, "External sensor %u unavailable", condition.externalSensorId);
      }
      return false;
    }
    bool result = schedulerCompare(actual, condition.compare, condition.value);
    char part[96];
    if (condition.source == SCHEDULER_SOURCE_LOCAL) {
      snprintf(part, sizeof(part), "%s %.2f %s %.2f -> %s",
        conditionName(condition.field), actual, operatorName(condition.compare),
        condition.value, result ? "true" : "false");
    } else {
      snprintf(part, sizeof(part), "External sensor %u %.2f %s %.2f -> %s",
        condition.externalSensorId, actual, operatorName(condition.compare),
        condition.value, result ? "true" : "false");
    }
    if (i > 0 && used + 5 < detailSize) {
      strlcpy(detail + used, " AND ", detailSize - used);
      used += 5;
    }
    if (used < detailSize) {
      strlcpy(detail + used, part, detailSize - used);
      used = strlen(detail);
    }
    if (!result) return false;
  }
  return true;
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
  pending_[index].action = entry.action;
  pending_[index].value = entry.actionValue;
  pending_[index].automation = entry.id == 0;
  strlcpy(pending_[index].conditionDetail, conditionDetail,
    sizeof(pending_[index].conditionDetail));
  pending_[index].guard = guard;
  pending_[index].observer = observer;
  pending_[index].observerContext = observerContext;
  pendingCount_++;
  snprintf(message, messageSize, "Action queued");
  return true;
}

void SchedulerManager::dispatchNext() {
  if (pendingCount_ == 0 || dispatcher_ == nullptr) return;
  unsigned long now = millis();
  if (!schedulerDispatchReady(pendingCount_, lastDispatchAt_, now,
      SCHEDULER_DISPATCH_INTERVAL_MS)) return;

  PendingAction pending = pending_[pendingStart_];
  pendingStart_ = (uint8_t)((pendingStart_ + 1) % SCHEDULER_MAX_ENTRIES);
  pendingCount_--;
  lastDispatchAt_ = now;

  SchedulerEntry eventEntry = {};
  eventEntry.id = pending.entryId;
  eventEntry.action = pending.action;
  eventEntry.actionValue = pending.value;
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
    result = dispatcher_(pending.action, pending.value, actionDetail, sizeof(actionDetail));
  }
  char detail[SCHEDULER_DETAIL_LENGTH] = {0};
  snprintf(detail, sizeof(detail), "%s; %s", pending.conditionDetail, actionDetail);
  switch (result) {
    case SCHEDULER_DISPATCH_EXECUTED: addEvent(eventEntry, "executed", detail); break;
    case SCHEDULER_DISPATCH_NO_CHANGE: addEvent(eventEntry, "no change", detail); break;
    case SCHEDULER_DISPATCH_BUSY: addEvent(eventEntry, "busy", detail); break;
    default: addEvent(eventEntry, "failed", detail); break;
  }
  if (pending.observer != nullptr) {
    pending.observer(result, actionDetail, pending.observerContext);
  }
}

void SchedulerManager::cancelPending(const char *reason) {
  while (pendingCount_ > 0) {
    PendingAction pending = pending_[pendingStart_];
    pendingStart_ = (uint8_t)((pendingStart_ + 1) % SCHEDULER_MAX_ENTRIES);
    pendingCount_--;
    SchedulerEntry eventEntry = {};
    eventEntry.id = pending.entryId;
    eventEntry.action = pending.action;
    eventEntry.actionValue = pending.value;
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
  if (entry.action >= SCHEDULER_ACTION_COUNT) {
    snprintf(message, messageSize, "Unknown action"); return false;
  }
  switch (entry.action) {
    case SCHEDULER_ACTION_SET_OPERATION_MODE:
      if (entry.actionValue < 0 || entry.actionValue > 6) { snprintf(message, messageSize, "Operating mode must be 0..6"); return false; }
      break;
    case SCHEDULER_ACTION_SET_DHW_TARGET:
      if (entry.actionValue < 40 || entry.actionValue > 75) { snprintf(message, messageSize, "DHW target must be 40..75 C"); return false; }
      break;
    case SCHEDULER_ACTION_SET_Z1_REQUEST:
      if (entry.actionValue < -5 || entry.actionValue > 65) { snprintf(message, messageSize, "Zone 1 request must be -5..65 C"); return false; }
      break;
    case SCHEDULER_ACTION_SET_QUIET_MODE:
      if (entry.actionValue < 0 || entry.actionValue > 3) { snprintf(message, messageSize, "Quiet mode must be 0..3"); return false; }
      break;
    default: break;
  }
  if (entry.conditionCount > SCHEDULER_MAX_CONDITIONS) {
    snprintf(message, messageSize, "At most %u conditions are supported", SCHEDULER_MAX_CONDITIONS);
    return false;
  }
  for (uint8_t i = 0; i < entry.conditionCount; i++) {
    const SchedulerCondition &condition = entry.conditions[i];
    if (condition.source >= SCHEDULER_SOURCE_COUNT ||
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
  entry.actionValue = (int16_t)actionValue;
  entry.lastExecutionKey = UINT32_MAX;

  if (!parseAction(object["action"] | "", entry.action)) {
    snprintf(message, messageSize, "Unknown action"); return false;
  }
  JsonArrayConst conditions = object["conditions"].as<JsonArrayConst>();
  if (!conditions.isNull()) {
    for (JsonObjectConst conditionObject : conditions) {
      if (entry.conditionCount >= SCHEDULER_MAX_CONDITIONS) {
        snprintf(message, messageSize, "At most %u conditions are supported", SCHEDULER_MAX_CONDITIONS);
        return false;
      }
      SchedulerCondition &condition = entry.conditions[entry.conditionCount];
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
  char detail[SCHEDULER_DETAIL_LENGTH];
  if (!evaluateCondition(entries_[index], detail, sizeof(detail))) {
    addEvent(entries_[index], "skipped", detail);
    snprintf(message, messageSize, "Condition is false; action skipped");
    return true;
  }
  return enqueue(entries_[index], detail, nullptr, nullptr, nullptr,
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
  entry.action = action;
  entry.actionValue = value;
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
  if (error || (version != 1 && version != SCHEDULER_CONFIG_VERSION)) {
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
  object["action"] = actionName(entry.action);
  object["actionValue"] = entry.actionValue;
  JsonArray conditions = object["conditions"].to<JsonArray>();
  for (uint8_t i = 0; i < entry.conditionCount; i++) {
    const SchedulerCondition &condition = entry.conditions[i];
    JsonObject conditionObject = conditions.add<JsonObject>();
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
  document["pendingActions"] = pendingCount_;
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
    "set_operation_mode", "set_dhw_target", "set_z1_request", "set_quiet_mode"};
  return action < SCHEDULER_ACTION_COUNT ? names[action] : "unknown";
}

const char *SchedulerManager::conditionName(SchedulerConditionField field) {
  static const char *names[] = {"none", "dhw_temperature", "outside_temperature",
    "room_temperature", "main_inlet_temperature", "main_outlet_temperature"};
  return field < SCHEDULER_CONDITION_COUNT ? names[field] : "unknown";
}

const char *SchedulerManager::sourceName(SchedulerConditionSource source) {
  return source == SCHEDULER_SOURCE_MQTT ? "mqtt" : "local";
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

uint8_t SchedulerManager::conditionTopic(SchedulerConditionField field) {
  switch (field) {
    case SCHEDULER_CONDITION_DHW_TEMPERATURE: return 10;
    case SCHEDULER_CONDITION_OUTSIDE_TEMPERATURE: return 14;
    case SCHEDULER_CONDITION_ROOM_TEMPERATURE: return 56;
    case SCHEDULER_CONDITION_MAIN_INLET_TEMPERATURE: return 5;
    case SCHEDULER_CONDITION_MAIN_OUTLET_TEMPERATURE: return 6;
    default: return 255;
  }
}
