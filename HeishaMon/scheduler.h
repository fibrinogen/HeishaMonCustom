#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "scheduler_logic.h"

#define SCHEDULER_CONFIG_VERSION 1
#define SCHEDULER_MAX_ENTRIES 16
#define SCHEDULER_MAX_EVENTS 10
#define SCHEDULER_NAME_LENGTH 33
#define SCHEDULER_DETAIL_LENGTH 128

enum SchedulerActionType : uint8_t {
  SCHEDULER_ACTION_FORCE_DHW = 0,
  SCHEDULER_ACTION_HEATPUMP_ON,
  SCHEDULER_ACTION_HEATPUMP_OFF,
  SCHEDULER_ACTION_SET_OPERATION_MODE,
  SCHEDULER_ACTION_SET_DHW_TARGET,
  SCHEDULER_ACTION_SET_Z1_REQUEST,
  SCHEDULER_ACTION_SET_QUIET_MODE,
  SCHEDULER_ACTION_COUNT
};

enum SchedulerConditionField : uint8_t {
  SCHEDULER_CONDITION_NONE = 0,
  SCHEDULER_CONDITION_DHW_TEMPERATURE,
  SCHEDULER_CONDITION_OUTSIDE_TEMPERATURE,
  SCHEDULER_CONDITION_ROOM_TEMPERATURE,
  SCHEDULER_CONDITION_MAIN_INLET_TEMPERATURE,
  SCHEDULER_CONDITION_MAIN_OUTLET_TEMPERATURE,
  SCHEDULER_CONDITION_COUNT
};

enum SchedulerDispatchResult : uint8_t {
  SCHEDULER_DISPATCH_EXECUTED = 0,
  SCHEDULER_DISPATCH_NO_CHANGE,
  SCHEDULER_DISPATCH_BUSY,
  SCHEDULER_DISPATCH_FAILED
};

struct SchedulerEntry {
  uint8_t id;
  bool enabled;
  char name[SCHEDULER_NAME_LENGTH];
  uint8_t dayMask;
  uint8_t hour;
  uint8_t minute;
  SchedulerActionType action;
  int16_t actionValue;
  SchedulerConditionField conditionField;
  SchedulerCompareOperator conditionOperator;
  float conditionValue;
  uint32_t lastExecutionKey;
};

struct SchedulerEvent {
  char timestamp[20];
  uint8_t entryId;
  char name[SCHEDULER_NAME_LENGTH];
  char result[16];
  char detail[SCHEDULER_DETAIL_LENGTH];
};

typedef bool (*SchedulerStateReader)(uint8_t topic, float *value);
typedef SchedulerDispatchResult (*SchedulerActionDispatcher)(SchedulerActionType action,
  int16_t value, char *detail, size_t detailSize);
typedef void (*SchedulerLogger)(char *message);
typedef void (*SchedulerDispatchObserver)(SchedulerDispatchResult result,
  const char *detail, void *context);
typedef bool (*SchedulerDispatchGuard)(void *context);

class SchedulerManager {
 public:
  SchedulerManager();
  void begin(SchedulerStateReader stateReader, SchedulerActionDispatcher dispatcher,
    SchedulerLogger logger);
  void loop();

  bool load();
  bool save();
  bool upsert(JsonObjectConst object, char *message, size_t messageSize);
  bool remove(uint8_t id, char *message, size_t messageSize);
  bool runNow(uint8_t id, char *message, size_t messageSize);
  bool setEnabled(bool enabled, char *message, size_t messageSize);
  bool claimDue(bool enabled, uint8_t dayMask, uint8_t hour, uint8_t minute,
    uint32_t &lastExecutionKey) const;
  bool submitAutomationAction(const char *name, SchedulerActionType action, int16_t value,
    const char *reason, SchedulerDispatchGuard guard,
    SchedulerDispatchObserver observer, void *context,
    char *message, size_t messageSize);
  void toJson(JsonDocument &document) const;

  bool isEnabled() const { return enabled_; }
  uint8_t count() const { return count_; }
  uint8_t enabledCount() const;
  bool clockValid() const;

  static const char *actionName(SchedulerActionType action);
  static const char *conditionName(SchedulerConditionField field);
  static const char *operatorName(SchedulerCompareOperator op);

 private:
  struct PendingAction {
    uint8_t entryId;
    char name[SCHEDULER_NAME_LENGTH];
    SchedulerActionType action;
    int16_t value;
    bool automation;
    char conditionDetail[80];
    SchedulerDispatchGuard guard;
    SchedulerDispatchObserver observer;
    void *observerContext;
  };

  SchedulerEntry entries_[SCHEDULER_MAX_ENTRIES];
  SchedulerEvent events_[SCHEDULER_MAX_EVENTS];
  PendingAction pending_[SCHEDULER_MAX_ENTRIES];
  uint8_t count_;
  uint8_t eventStart_;
  uint8_t eventCount_;
  uint8_t pendingStart_;
  uint8_t pendingCount_;
  bool enabled_;
  bool clockWasValid_;
  uint32_t lastCheckedMinuteKey_;
  unsigned long lastDispatchAt_;
  SchedulerStateReader stateReader_;
  SchedulerActionDispatcher dispatcher_;
  SchedulerLogger logger_;

  bool readClock(SchedulerClock &clock) const;
  void checkSchedules(const SchedulerClock &clock);
  bool evaluateCondition(const SchedulerEntry &entry, char *detail, size_t detailSize);
  bool enqueue(const SchedulerEntry &entry, const char *conditionDetail,
    SchedulerDispatchGuard guard, SchedulerDispatchObserver observer, void *observerContext,
    char *message, size_t messageSize);
  void dispatchNext();
  void cancelPending(const char *reason);
  void addEvent(const SchedulerEntry &entry, const char *result, const char *detail);
  void log(const char *message) const;
  int8_t findIndex(uint8_t id) const;
  uint8_t nextId() const;
  bool parseEntry(JsonObjectConst object, SchedulerEntry &entry,
    char *message, size_t messageSize) const;
  bool validateEntry(const SchedulerEntry &entry, char *message, size_t messageSize) const;
  static bool parseAction(const char *name, SchedulerActionType &action);
  static bool parseCondition(const char *name, SchedulerConditionField &field);
  static bool parseOperator(const char *name, SchedulerCompareOperator &op);
  static uint8_t conditionTopic(SchedulerConditionField field);
  void entryToJson(const SchedulerEntry &entry, JsonObject object) const;
};
