#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "scheduler.h"
#include "smart_dhw_logic.h"

#define SMART_DHW_CONFIG_VERSION 1
#define SMART_DHW_MAX_EVENTS 10

enum SmartDhwSlot : uint8_t {
  SMART_DHW_SLOT_EVENING = 0,
  SMART_DHW_SLOT_MORNING
};

enum SmartDhwState : uint8_t {
  SMART_DHW_STATE_IDLE = 0,
  SMART_DHW_STATE_REQUESTED,
  SMART_DHW_STATE_ACTIVE,
  SMART_DHW_STATE_ERROR
};

struct SmartDhwConfig {
  bool enabled;
  bool eveningEnabled;
  uint8_t eveningHour;
  uint8_t eveningMinute;
  float eveningTriggerTemp;
  bool morningEnabled;
  uint8_t morningHour;
  uint8_t morningMinute;
  float morningTriggerTemp;
  uint16_t minimumIntervalMinutes;
};

struct SmartDhwEvent {
  char timestamp[20];
  char reserve[20];
  char result[24];
  char detail[128];
};

class SmartDhwController {
 public:
  SmartDhwController();
  void begin(SchedulerManager *scheduler, SchedulerStateReader stateReader,
    SchedulerLogger logger);
  void loop();

  bool load();
  bool save();
  bool update(JsonObjectConst object, char *message, size_t messageSize);
  bool testDecision(SmartDhwSlot slot, char *message, size_t messageSize);
  void toJson(JsonDocument &document) const;

  const SmartDhwConfig &config() const { return config_; }
  SmartDhwState state() const { return state_; }

 private:
  SmartDhwConfig config_;
  SchedulerManager *scheduler_;
  SchedulerStateReader stateReader_;
  SchedulerLogger logger_;
  SmartDhwState state_;
  uint32_t lastEveningKey_;
  uint32_t lastMorningKey_;
  time_t lastSuccessfulStart_;
  unsigned long requestStartedAt_;
  SmartDhwSlot requestedSlot_;
  SmartDhwEvent events_[SMART_DHW_MAX_EVENTS];
  uint8_t eventStart_;
  uint8_t eventCount_;

  bool configValid(const SmartDhwConfig &config) const;
  bool readValue(uint8_t topic, float &value) const;
  SmartDhwDecisionCode evaluate(SmartDhwSlot slot, float &temperature,
    float &threshold, char *detail, size_t detailSize) const;
  bool runCheck(SmartDhwSlot slot, bool testOnly, char *message, size_t messageSize);
  void handleDispatchResult(SchedulerDispatchResult result, const char *detail);
  static void dispatchObserver(SchedulerDispatchResult result, const char *detail,
    void *context);
  static bool dispatchGuard(void *context);
  void addEvent(SmartDhwSlot slot, const char *result, const char *detail);
  void log(const char *message) const;
  void writeConfig(JsonDocument &document) const;
  void writeNextCheck(JsonObject object) const;
  void writeSlotNextCheck(JsonObject object, SmartDhwSlot slot) const;
  static const char *slotName(SmartDhwSlot slot);
  static const char *stateName(SmartDhwState state);
  static const char *decisionName(SmartDhwDecisionCode decision);
};
