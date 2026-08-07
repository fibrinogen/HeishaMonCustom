#include "scheduler.h"

#include <LittleFS.h>
#include <time.h>

static const char *SCHEDULER_FILE = "/scheduler.json";
static const unsigned long SCHEDULER_DISPATCH_INTERVAL_MS = 2000UL;

SchedulerManager::SchedulerManager()
  : count_(0), eventStart_(0), eventCount_(0), pendingStart_(0), pendingCount_(0),
    enabled_(true), clockWasValid_(false), lastCheckedMinuteKey_(UINT32_MAX),
    lastDispatchAt_(0), stateReader_(nullptr), dispatcher_(nullptr), logger_(nullptr) {
  memset(entries_, 0, sizeof(entries_));
  memset(events_, 0, sizeof(events_));
  memset(pending_, 0, sizeof(pending_));
}

void SchedulerManager::begin(SchedulerStateReader stateReader,
    SchedulerActionDispatcher dispatcher, SchedulerLogger logger) {
  stateReader_ = stateReader;
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
  if (entry.conditionField == SCHEDULER_CONDITION_NONE) {
    snprintf(detail, detailSize, "No condition");
    return true;
  }
  if (stateReader_ == nullptr) {
    snprintf(detail, detailSize, "Condition state reader unavailable");
    return false;
  }
  float actual = 0;
  if (!stateReader_(conditionTopic(entry.conditionField), &actual) || !isfinite(actual)) {
    snprintf(detail, detailSize, "%s unavailable", conditionName(entry.conditionField));
    return false;
  }
  bool result = schedulerCompare(actual, entry.conditionOperator, entry.conditionValue);
  snprintf(detail, detailSize, "%s %.2f %s %.2f -> %s",
    conditionName(entry.conditionField), actual, operatorName(entry.conditionOperator),
    entry.conditionValue, result ? "true" : "false");
  return result;
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
  if (entry.conditionField >= SCHEDULER_CONDITION_COUNT ||
      entry.conditionOperator > SCHEDULER_COMPARE_GREATER || !isfinite(entry.conditionValue) ||
      entry.conditionValue < -100.0f || entry.conditionValue > 200.0f) {
    snprintf(message, messageSize, "Invalid condition"); return false;
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
  entry.conditionValue = object["conditionValue"] | 0.0f;
  entry.lastExecutionKey = UINT32_MAX;

  if (!parseAction(object["action"] | "", entry.action)) {
    snprintf(message, messageSize, "Unknown action"); return false;
  }
  if (!parseCondition(object["conditionField"] | "none", entry.conditionField)) {
    snprintf(message, messageSize, "Unknown condition field"); return false;
  }
  if (!parseOperator(object["conditionOperator"] | "<", entry.conditionOperator)) {
    snprintf(message, messageSize, "Unknown condition operator"); return false;
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
    if (count_ >= SCHEDULER_MAX_ENTRIES) { snprintf(message, messageSize, "Maximum of 16 schedules reached"); return false; }
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
  if (error || (document["version"] | 0) != SCHEDULER_CONFIG_VERSION) {
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
  object["conditionField"] = conditionName(entry.conditionField);
  object["conditionOperator"] = operatorName(entry.conditionOperator);
  object["conditionValue"] = entry.conditionValue;
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
