#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "scheduler_logic.h"

#define SCHEDULER_CONFIG_VERSION 5
#define SCHEDULER_MAX_ENTRIES 24
#define SCHEDULER_MAX_EVENTS 10
#define SCHEDULER_NAME_LENGTH 33
#define SCHEDULER_DETAIL_LENGTH 128
#define SCHEDULER_MAX_ACTIONS 4
#define SCHEDULER_MAX_CONDITIONS 4

enum SchedulerActionType : uint8_t {
  SCHEDULER_ACTION_FORCE_DHW = 0,
  SCHEDULER_ACTION_HEATPUMP_ON,
  SCHEDULER_ACTION_HEATPUMP_OFF,
  SCHEDULER_ACTION_SET_OPERATION_MODE,
  SCHEDULER_ACTION_SET_DHW_TARGET,
  SCHEDULER_ACTION_SET_HEAT_CURVE_SHIFT,
  SCHEDULER_ACTION_SET_Z1_HEATING_WATER_TARGET,
  SCHEDULER_ACTION_SET_Z1_ROOM_TARGET,
  // Kept only so old files can be read and safely marked for review.
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
  SCHEDULER_CONDITION_THREE_WAY_VALVE,
  SCHEDULER_CONDITION_FORCE_DHW,
  SCHEDULER_CONDITION_COUNT
};

enum SchedulerConditionSource : uint8_t {
  SCHEDULER_SOURCE_LOCAL = 0,
  SCHEDULER_SOURCE_MQTT,
  SCHEDULER_SOURCE_COUNT
};

struct SchedulerAction {
  SchedulerActionType type;
  int16_t value;
};

struct SchedulerCondition {
  SchedulerConditionJoin join;
  SchedulerConditionSource source;
  SchedulerConditionField field;
  uint8_t externalSensorId;
  SchedulerCompareOperator compare;
  float value;
};

struct SchedulerEntry {
  uint8_t id;
  bool enabled;
  char name[SCHEDULER_NAME_LENGTH];
  uint8_t dayMask;
  uint8_t hour;
  uint8_t minute;
  uint8_t actionCount;
  SchedulerAction actions[SCHEDULER_MAX_ACTIONS];
  uint8_t conditionCount;
  SchedulerCondition conditions[SCHEDULER_MAX_CONDITIONS];
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
typedef bool (*SchedulerValueReader)(SchedulerConditionSource source, uint8_t sourceId,
  float *value, uint32_t *ageSeconds, char *detail, size_t detailSize);
typedef SchedulerDispatchResult (*SchedulerActionDispatcher)(SchedulerActionType action,
  int16_t value, char *detail, size_t detailSize);
typedef void (*SchedulerLogger)(char *message);
typedef void (*SchedulerPersistentEventLogger)(const SchedulerEvent *event);
typedef void (*SchedulerDispatchObserver)(SchedulerDispatchResult result,
  const char *detail, void *context);
typedef bool (*SchedulerDispatchGuard)(void *context);

class SchedulerManager {
 public:
  SchedulerManager();
  void begin(SchedulerValueReader valueReader, SchedulerActionDispatcher dispatcher,
    SchedulerLogger logger);
  void setPersistentEventLogger(SchedulerPersistentEventLogger logger) {
    persistentEventLogger_ = logger;
  }
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
  struct PendingActionGroup {
    uint8_t entryId;
    char name[SCHEDULER_NAME_LENGTH];
    uint8_t actionCount;
    uint8_t nextActionIndex;
    SchedulerAction actions[SCHEDULER_MAX_ACTIONS];
    bool automation;
    char conditionDetail[80];
    SchedulerDispatchGuard guard;
    SchedulerDispatchObserver observer;
    void *observerContext;
  };

  SchedulerEntry entries_[SCHEDULER_MAX_ENTRIES];
  SchedulerEvent events_[SCHEDULER_MAX_EVENTS];
  PendingActionGroup pending_[SCHEDULER_MAX_ENTRIES];
  uint8_t count_;
  uint8_t eventStart_;
  uint8_t eventCount_;
  uint8_t pendingStart_;
  uint8_t pendingCount_;
  bool enabled_;
  bool clockWasValid_;
  bool clockInvalidEventRecorded_;
  uint32_t lastCheckedMinuteKey_;
  unsigned long lastDispatchAt_;
  SchedulerValueReader valueReader_;
  SchedulerActionDispatcher dispatcher_;
  SchedulerLogger logger_;
  SchedulerPersistentEventLogger persistentEventLogger_;

  bool readClock(SchedulerClock &clock) const;
  void checkSchedules(const SchedulerClock &clock);
  bool evaluateCondition(const SchedulerEntry &entry, char *detail, size_t detailSize);
  bool enqueue(const SchedulerEntry &entry, const char *conditionDetail,
    SchedulerDispatchGuard guard, SchedulerDispatchObserver observer, void *observerContext,
    char *message, size_t messageSize);
  void dispatchNext();
  uint8_t pendingActionCount() const;
  void cancelPending(const char *reason);
  void addEvent(const SchedulerEntry &entry, const char *result, const char *detail);
  void log(const char *message) const;
  int8_t findIndex(uint8_t id) const;
  uint8_t nextId() const;
  bool parseEntry(JsonObjectConst object, SchedulerEntry &entry,
    char *message, size_t messageSize) const;
  bool validateEntry(const SchedulerEntry &entry, char *message, size_t messageSize) const;
  static bool validateAction(const SchedulerAction &action,
    char *message, size_t messageSize);
  static bool parseAction(const char *name, SchedulerActionType &action);
  static bool parseCondition(const char *name, SchedulerConditionField &field);
  static bool parseSource(const char *name, SchedulerConditionSource &source);
  static bool parseOperator(const char *name, SchedulerCompareOperator &op);
  static bool parseJoin(const char *name, SchedulerConditionJoin &join);
  static uint8_t conditionTopic(SchedulerConditionField field);
  static const char *conditionDisplayName(SchedulerConditionField field);
  static void describeAction(const SchedulerAction &action, char *description,
    size_t descriptionSize);
  static void describeActions(const SchedulerEntry &entry, char *description,
    size_t descriptionSize);
  static const char *joinName(SchedulerConditionJoin join);
  static const char *sourceName(SchedulerConditionSource source);
  void entryToJson(const SchedulerEntry &entry, JsonObject object) const;
};
