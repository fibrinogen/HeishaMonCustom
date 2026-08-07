#include "smart_dhw.h"

#include <LittleFS.h>
#include <time.h>

static const char *SMART_DHW_FILE = "/smartdhw.json";
static const unsigned long SMART_DHW_REQUEST_TIMEOUT_MS = 120000UL;

SmartDhwController::SmartDhwController()
  : scheduler_(nullptr), stateReader_(nullptr), logger_(nullptr),
    state_(SMART_DHW_STATE_IDLE), lastEveningKey_(UINT32_MAX),
    lastMorningKey_(UINT32_MAX), lastSuccessfulStart_(0), requestStartedAt_(0),
    requestedSlot_(SMART_DHW_SLOT_EVENING), eventStart_(0), eventCount_(0) {
  config_ = {false, true, 18, 0, 42.0f, true, 4, 0, 38.0f, 60};
  memset(events_, 0, sizeof(events_));
}

void SmartDhwController::begin(SchedulerManager *scheduler,
    SchedulerStateReader stateReader, SchedulerLogger logger) {
  scheduler_ = scheduler;
  stateReader_ = stateReader;
  logger_ = logger;
  if (!load()) log("[SMART_DHW] invalid configuration; feature disabled");
}

bool SmartDhwController::configValid(const SmartDhwConfig &config) const {
  return smartDhwConfigValuesValid(config.eveningHour, config.eveningMinute,
    config.eveningTriggerTemp, config.morningHour, config.morningMinute,
    config.morningTriggerTemp, config.minimumIntervalMinutes);
}

bool SmartDhwController::readValue(uint8_t topic, float &value) const {
  return stateReader_ != nullptr && stateReader_(topic, &value) && isfinite(value);
}

void SmartDhwController::loop() {
  if (scheduler_ == nullptr) return;

  if (scheduler_->claimDue(config_.enabled && config_.eveningEnabled, 0x7F,
      config_.eveningHour, config_.eveningMinute, lastEveningKey_)) {
    char message[128];
    runCheck(SMART_DHW_SLOT_EVENING, false, message, sizeof(message));
  }
  if (scheduler_->claimDue(config_.enabled && config_.morningEnabled, 0x7F,
      config_.morningHour, config_.morningMinute, lastMorningKey_)) {
    char message[128];
    runCheck(SMART_DHW_SLOT_MORNING, false, message, sizeof(message));
  }

  if (state_ != SMART_DHW_STATE_REQUESTED && state_ != SMART_DHW_STATE_ACTIVE) return;
  float force = 0;
  float valve = 0;
  bool statesAvailable = readValue(2, force) && readValue(20, valve);
  bool active = statesAvailable && (lroundf(force) != 0 || lroundf(valve) == 1);
  if (state_ == SMART_DHW_STATE_REQUESTED && active) {
    state_ = SMART_DHW_STATE_ACTIVE;
  } else if (state_ == SMART_DHW_STATE_REQUESTED &&
      (unsigned long)(millis() - requestStartedAt_) >= SMART_DHW_REQUEST_TIMEOUT_MS) {
    state_ = SMART_DHW_STATE_ERROR;
    addEvent(requestedSlot_, "Timeout", "Force DHW did not become active within 120 seconds");
  } else if (state_ == SMART_DHW_STATE_ACTIVE && statesAvailable && !active) {
    state_ = SMART_DHW_STATE_IDLE;
    addEvent(requestedSlot_, "Completed", "DHW operation is no longer active");
  }
}

SmartDhwDecisionCode SmartDhwController::evaluate(SmartDhwSlot slot,
    float &temperature, float &threshold, char *detail, size_t detailSize) const {
  bool reserveEnabled = slot == SMART_DHW_SLOT_EVENING ?
    config_.eveningEnabled : config_.morningEnabled;
  threshold = slot == SMART_DHW_SLOT_EVENING ?
    config_.eveningTriggerTemp : config_.morningTriggerTemp;

  time_t now = time(nullptr);
  struct tm local = {};
  bool clockValid = now > 0 && localtime_r(&now, &local) != nullptr &&
    local.tm_year + 1900 >= 2024;

  float installed = 0;
  float valve = 0;
  float force = 0;
  bool tempRead = readValue(10, temperature);
  bool stateAvailable = tempRead && readValue(100, installed) &&
    readValue(20, valve) && readValue(2, force);
  bool temperatureValid = tempRead && temperature >= 0.0f && temperature <= 100.0f;
  bool intervalBlocked = lastSuccessfulStart_ > 0 && now > lastSuccessfulStart_ &&
    (uint32_t)(now - lastSuccessfulStart_) <
      (uint32_t)config_.minimumIntervalMinutes * 60UL;

  SmartDhwDecisionInput input = {
    config_.enabled,
    reserveEnabled,
    clockValid,
    configValid(config_),
    stateAvailable,
    lroundf(installed) != 0,
    temperatureValid,
    lroundf(valve) == 1,
    lroundf(force) != 0,
    state_ == SMART_DHW_STATE_REQUESTED || state_ == SMART_DHW_STATE_ACTIVE,
    intervalBlocked,
    temperature,
    threshold
  };
  SmartDhwDecisionCode decision = smartDhwEvaluate(input);

  if (decision == SMART_DHW_DECISION_START ||
      decision == SMART_DHW_DECISION_SUFFICIENT) {
    snprintf(detail, detailSize, "DHW %.1f C %s threshold %.1f C",
      temperature, decision == SMART_DHW_DECISION_START ? "<" : ">=", threshold);
  } else if (decision == SMART_DHW_DECISION_MINIMUM_INTERVAL) {
    uint32_t elapsedMinutes = now > lastSuccessfulStart_ ?
      (uint32_t)(now - lastSuccessfulStart_) / 60UL : 0;
    snprintf(detail, detailSize, "Only %lu of %u minimum minutes elapsed",
      (unsigned long)elapsedMinutes, config_.minimumIntervalMinutes);
  } else {
    snprintf(detail, detailSize, "%s", decisionName(decision));
  }
  return decision;
}

bool SmartDhwController::runCheck(SmartDhwSlot slot, bool testOnly,
    char *message, size_t messageSize) {
  if (!testOnly && state_ == SMART_DHW_STATE_ERROR) state_ = SMART_DHW_STATE_IDLE;
  float temperature = NAN;
  float threshold = NAN;
  char detail[128] = {0};
  SmartDhwDecisionCode decision = evaluate(slot, temperature, threshold,
    detail, sizeof(detail));

  char logMessage[220];
  snprintf(logMessage, sizeof(logMessage), "[SMART_DHW] %s check: %s; %s",
    slotName(slot), decisionName(decision), detail);
  log(logMessage);

  if (testOnly) {
    char testDetail[128];
    snprintf(testDetail, sizeof(testDetail), "%s; %s", detail,
      decision == SMART_DHW_DECISION_START ? "charge WOULD be requested" :
      "no charge would be requested");
    addEvent(slot, "Test only", testDetail);
    snprintf(message, messageSize, "%s", testDetail);
    return true;
  }

  if (decision != SMART_DHW_DECISION_START) {
    addEvent(slot, decision == SMART_DHW_DECISION_SUFFICIENT ? "Skipped" : "Blocked", detail);
    snprintf(message, messageSize, "%s", detail);
    return true;
  }

  state_ = SMART_DHW_STATE_REQUESTED;
  requestedSlot_ = slot;
  requestStartedAt_ = millis();
  char queueMessage[96] = {0};
  bool queued = scheduler_->submitAutomationAction(
    slot == SMART_DHW_SLOT_EVENING ? "Smart DHW evening" : "Smart DHW morning",
    SCHEDULER_ACTION_FORCE_DHW, 1, detail, dispatchGuard, dispatchObserver, this,
    queueMessage, sizeof(queueMessage));
  if (!queued) {
    state_ = SMART_DHW_STATE_ERROR;
    addEvent(slot, "Failed", queueMessage);
    snprintf(message, messageSize, "%s", queueMessage);
    return false;
  }
  addEvent(slot, "Queued", detail);
  snprintf(message, messageSize, "Force DHW queued");
  return true;
}

void SmartDhwController::dispatchObserver(SchedulerDispatchResult result,
    const char *detail, void *context) {
  if (context != nullptr) {
    ((SmartDhwController *)context)->handleDispatchResult(result, detail);
  }
}

bool SmartDhwController::dispatchGuard(void *context) {
  if (context == nullptr) return false;
  SmartDhwController *controller = (SmartDhwController *)context;
  return controller->config_.enabled && controller->state_ == SMART_DHW_STATE_REQUESTED;
}

void SmartDhwController::handleDispatchResult(SchedulerDispatchResult result,
    const char *detail) {
  if (result == SCHEDULER_DISPATCH_EXECUTED) {
    state_ = SMART_DHW_STATE_REQUESTED;
    requestStartedAt_ = millis();
    lastSuccessfulStart_ = time(nullptr);
    addEvent(requestedSlot_, "Started", detail);
  } else if (result == SCHEDULER_DISPATCH_NO_CHANGE) {
    state_ = SMART_DHW_STATE_ACTIVE;
    addEvent(requestedSlot_, "Already active", detail);
  } else {
    state_ = SMART_DHW_STATE_ERROR;
    addEvent(requestedSlot_, result == SCHEDULER_DISPATCH_BUSY ? "Busy" : "Failed", detail);
  }
}

bool SmartDhwController::testDecision(SmartDhwSlot slot,
    char *message, size_t messageSize) {
  return runCheck(slot, true, message, messageSize);
}

void SmartDhwController::addEvent(SmartDhwSlot slot, const char *result,
    const char *detail) {
  uint8_t index;
  if (eventCount_ < SMART_DHW_MAX_EVENTS) {
    index = (uint8_t)((eventStart_ + eventCount_) % SMART_DHW_MAX_EVENTS);
    eventCount_++;
  } else {
    index = eventStart_;
    eventStart_ = (uint8_t)((eventStart_ + 1) % SMART_DHW_MAX_EVENTS);
  }
  SmartDhwEvent &event = events_[index];
  time_t now = time(nullptr);
  struct tm local = {};
  if (now > 0 && localtime_r(&now, &local) != nullptr && local.tm_year + 1900 >= 2024) {
    strftime(event.timestamp, sizeof(event.timestamp), "%Y-%m-%d %H:%M:%S", &local);
  } else {
    strlcpy(event.timestamp, "time unavailable", sizeof(event.timestamp));
  }
  strlcpy(event.reserve, slotName(slot), sizeof(event.reserve));
  strlcpy(event.result, result, sizeof(event.result));
  strlcpy(event.detail, detail, sizeof(event.detail));

  char logMessage[220];
  snprintf(logMessage, sizeof(logMessage), "[SMART_DHW] %s -> %s: %s",
    event.reserve, event.result, event.detail);
  log(logMessage);
}

void SmartDhwController::log(const char *message) const {
  if (logger_ != nullptr) logger_((char *)message);
}

bool SmartDhwController::load() {
  config_ = {false, true, 18, 0, 42.0f, true, 4, 0, 38.0f, 60};
  if (!LittleFS.begin() || !LittleFS.exists(SMART_DHW_FILE)) return true;
  File file = LittleFS.open(SMART_DHW_FILE, "r");
  if (!file) return false;
  JsonDocument document;
  DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error || (document["version"] | 0) != SMART_DHW_CONFIG_VERSION) {
    config_.enabled = false;
    return false;
  }
  long eveningHour = document["eveningHour"] | -1L;
  long eveningMinute = document["eveningMinute"] | -1L;
  long morningHour = document["morningHour"] | -1L;
  long morningMinute = document["morningMinute"] | -1L;
  long minimumInterval = document["minimumIntervalMinutes"] | -1L;
  if (eveningHour < 0 || eveningHour > 23 || eveningMinute < 0 || eveningMinute > 59 ||
      morningHour < 0 || morningHour > 23 || morningMinute < 0 || morningMinute > 59 ||
      minimumInterval < 0 || minimumInterval > UINT16_MAX) {
    config_.enabled = false;
    return false;
  }
  SmartDhwConfig loaded = {
    document["enabled"] | false,
    document["eveningEnabled"] | true,
    (uint8_t)eveningHour,
    (uint8_t)eveningMinute,
    document["eveningTriggerTemp"] | NAN,
    document["morningEnabled"] | true,
    (uint8_t)morningHour,
    (uint8_t)morningMinute,
    document["morningTriggerTemp"] | NAN,
    (uint16_t)minimumInterval
  };
  if (!configValid(loaded)) {
    config_.enabled = false;
    return false;
  }
  config_ = loaded;
  return true;
}

void SmartDhwController::writeConfig(JsonDocument &document) const {
  document["version"] = SMART_DHW_CONFIG_VERSION;
  document["enabled"] = config_.enabled;
  document["eveningEnabled"] = config_.eveningEnabled;
  document["eveningHour"] = config_.eveningHour;
  document["eveningMinute"] = config_.eveningMinute;
  document["eveningTriggerTemp"] = config_.eveningTriggerTemp;
  document["morningEnabled"] = config_.morningEnabled;
  document["morningHour"] = config_.morningHour;
  document["morningMinute"] = config_.morningMinute;
  document["morningTriggerTemp"] = config_.morningTriggerTemp;
  document["minimumIntervalMinutes"] = config_.minimumIntervalMinutes;
}

bool SmartDhwController::save() {
  if (!LittleFS.begin()) return false;
  JsonDocument document;
  writeConfig(document);
  File file = LittleFS.open(SMART_DHW_FILE, "w");
  if (!file) return false;
  bool ok = serializeJson(document, file) > 0;
  file.close();
  return ok;
}

bool SmartDhwController::update(JsonObjectConst object,
    char *message, size_t messageSize) {
  long eveningHour = object["eveningHour"] | -1L;
  long eveningMinute = object["eveningMinute"] | -1L;
  long morningHour = object["morningHour"] | -1L;
  long morningMinute = object["morningMinute"] | -1L;
  long minimumInterval = object["minimumIntervalMinutes"] | -1L;
  SmartDhwConfig candidate = {
    object["enabled"] | false,
    object["eveningEnabled"] | false,
    (uint8_t)eveningHour,
    (uint8_t)eveningMinute,
    object["eveningTriggerTemp"] | NAN,
    object["morningEnabled"] | false,
    (uint8_t)morningHour,
    (uint8_t)morningMinute,
    object["morningTriggerTemp"] | NAN,
    (uint16_t)minimumInterval
  };
  if (eveningHour < 0 || eveningHour > 23 || eveningMinute < 0 || eveningMinute > 59 ||
      morningHour < 0 || morningHour > 23 || morningMinute < 0 || morningMinute > 59 ||
      minimumInterval < 0 || minimumInterval > UINT16_MAX || !configValid(candidate)) {
    snprintf(message, messageSize, "Invalid Smart DHW configuration");
    return false;
  }
  SmartDhwConfig previous = config_;
  config_ = candidate;
  if (!save()) {
    config_ = previous;
    snprintf(message, messageSize, "Could not persist Smart DHW configuration");
    return false;
  }
  if (!config_.enabled) state_ = SMART_DHW_STATE_IDLE;
  snprintf(message, messageSize, "Smart DHW configuration saved");
  return true;
}

void SmartDhwController::writeNextCheck(JsonObject object) const {
  object["available"] = false;
  if (!config_.enabled) return;
  time_t now = time(nullptr);
  struct tm local = {};
  if (now <= 0 || localtime_r(&now, &local) == nullptr || local.tm_year + 1900 < 2024) return;

  time_t best = 0;
  SmartDhwSlot bestSlot = SMART_DHW_SLOT_EVENING;
  for (uint8_t slotValue = 0; slotValue < 2; slotValue++) {
    SmartDhwSlot slot = (SmartDhwSlot)slotValue;
    bool enabled = slot == SMART_DHW_SLOT_EVENING ? config_.eveningEnabled : config_.morningEnabled;
    if (!enabled) continue;
    struct tm candidate = local;
    candidate.tm_hour = slot == SMART_DHW_SLOT_EVENING ? config_.eveningHour : config_.morningHour;
    candidate.tm_min = slot == SMART_DHW_SLOT_EVENING ? config_.eveningMinute : config_.morningMinute;
    candidate.tm_sec = 0;
    time_t candidateTime = mktime(&candidate);
    if (candidateTime <= now) {
      candidate.tm_mday++;
      candidateTime = mktime(&candidate);
    }
    if (best == 0 || candidateTime < best) {
      best = candidateTime;
      bestSlot = slot;
    }
  }
  if (best == 0) return;
  struct tm bestLocal = {};
  localtime_r(&best, &bestLocal);
  char timestamp[20];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", &bestLocal);
  object["available"] = true;
  object["time"] = timestamp;
  object["reserve"] = slotName(bestSlot);
}

void SmartDhwController::writeSlotNextCheck(JsonObject object, SmartDhwSlot slot) const {
  bool slotEnabled = slot == SMART_DHW_SLOT_EVENING ?
    config_.eveningEnabled : config_.morningEnabled;
  object["enabled"] = config_.enabled && slotEnabled;
  object["available"] = false;
  if (!config_.enabled || !slotEnabled) return;
  time_t now = time(nullptr);
  struct tm candidate = {};
  if (now <= 0 || localtime_r(&now, &candidate) == nullptr ||
      candidate.tm_year + 1900 < 2024) return;
  candidate.tm_hour = slot == SMART_DHW_SLOT_EVENING ?
    config_.eveningHour : config_.morningHour;
  candidate.tm_min = slot == SMART_DHW_SLOT_EVENING ?
    config_.eveningMinute : config_.morningMinute;
  candidate.tm_sec = 0;
  time_t candidateTime = mktime(&candidate);
  if (candidateTime <= now) {
    candidate.tm_mday++;
    candidateTime = mktime(&candidate);
  }
  localtime_r(&candidateTime, &candidate);
  char timestamp[20];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", &candidate);
  object["available"] = true;
  object["time"] = timestamp;
}

void SmartDhwController::toJson(JsonDocument &document) const {
  JsonObject config = document["config"].to<JsonObject>();
  config["version"] = SMART_DHW_CONFIG_VERSION;
  config["enabled"] = config_.enabled;
  config["eveningEnabled"] = config_.eveningEnabled;
  config["eveningHour"] = config_.eveningHour;
  config["eveningMinute"] = config_.eveningMinute;
  config["eveningTriggerTemp"] = config_.eveningTriggerTemp;
  config["morningEnabled"] = config_.morningEnabled;
  config["morningHour"] = config_.morningHour;
  config["morningMinute"] = config_.morningMinute;
  config["morningTriggerTemp"] = config_.morningTriggerTemp;
  config["minimumIntervalMinutes"] = config_.minimumIntervalMinutes;

  JsonObject status = document["status"].to<JsonObject>();
  status["state"] = stateName(state_);
  float value = 0;
  if (readValue(10, value)) status["dhwTemperature"] = value;
  else status["dhwTemperature"] = nullptr;
  if (readValue(9, value)) status["dhwTarget"] = value;
  else status["dhwTarget"] = nullptr;
  if (readValue(2, value)) status["forceDhwActive"] = lroundf(value) != 0;
  else status["forceDhwActive"] = nullptr;
  if (readValue(20, value)) status["dhwOperationActive"] = lroundf(value) == 1;
  else status["dhwOperationActive"] = nullptr;
  if (readValue(100, value)) status["dhwInstalled"] = lroundf(value) != 0;
  else status["dhwInstalled"] = nullptr;

  char lastStart[20] = "Never";
  struct tm local = {};
  if (lastSuccessfulStart_ > 0 && localtime_r(&lastSuccessfulStart_, &local) != nullptr) {
    strftime(lastStart, sizeof(lastStart), "%Y-%m-%d %H:%M:%S", &local);
  }
  status["lastSuccessfulStart"] = lastStart;
  JsonObject nextCheck = document["nextCheck"].to<JsonObject>();
  writeNextCheck(nextCheck);
  writeSlotNextCheck(nextCheck["evening"].to<JsonObject>(), SMART_DHW_SLOT_EVENING);
  writeSlotNextCheck(nextCheck["morning"].to<JsonObject>(), SMART_DHW_SLOT_MORNING);

  JsonArray events = document["events"].to<JsonArray>();
  for (uint8_t offset = 0; offset < eventCount_; offset++) {
    uint8_t index = (uint8_t)((eventStart_ + eventCount_ - 1 - offset) % SMART_DHW_MAX_EVENTS);
    JsonObject event = events.add<JsonObject>();
    event["time"] = events_[index].timestamp;
    event["reserve"] = events_[index].reserve;
    event["result"] = events_[index].result;
    event["detail"] = events_[index].detail;
  }
}

const char *SmartDhwController::slotName(SmartDhwSlot slot) {
  return slot == SMART_DHW_SLOT_EVENING ? "Evening reserve" : "Morning reserve";
}

const char *SmartDhwController::stateName(SmartDhwState state) {
  static const char *names[] = {"Idle", "Requested", "DHW active", "Error"};
  return state <= SMART_DHW_STATE_ERROR ? names[state] : "Unknown";
}

const char *SmartDhwController::decisionName(SmartDhwDecisionCode decision) {
  static const char *names[] = {
    "Charge required", "Sufficient hot water", "Smart DHW disabled",
    "Reserve disabled", "Local time invalid", "Configuration invalid",
    "Panasonic state unavailable", "DHW not installed", "DHW temperature invalid",
    "DHW operation already active", "Force DHW already active",
    "Smart DHW action already running", "Minimum start interval active"
  };
  return decision <= SMART_DHW_DECISION_MINIMUM_INTERVAL ? names[decision] : "Unknown decision";
}
