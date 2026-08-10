#include "custom_features.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <time.h>

#include "scheduler.h"
#include "smart_dhw.h"
#include "external_sensors.h"
#include "diagnostics_history.h"
#include "webfunctions.h"
#include "zone1_heat_semantics.h"
#include "heating_curve_shift.h"

#define DATASIZE 203
#define NUMBER_OF_TOPICS 144

String getDataValue(char *data, unsigned int topicNumber);
unsigned int set_heatpump_state(char *msg, unsigned char *cmd, char *logMsg);
unsigned int set_quiet_mode(char *msg, unsigned char *cmd, char *logMsg);
unsigned int set_force_DHW(char *msg, unsigned char *cmd, char *logMsg);
unsigned int set_force_sterilization(char *msg, unsigned char *cmd, char *logMsg);
unsigned int set_operation_mode(char *msg, unsigned char *cmd, char *logMsg);
unsigned int set_DHW_temp(char *msg, unsigned char *cmd, char *logMsg);
unsigned int set_z1_heat_request_temperature(char *msg, unsigned char *cmd, char *logMsg);

extern settingsStruct heishamonSettings;
extern char actData[DATASIZE];
extern unsigned long lastHeatpumpDataAt;

extern void log_message(char *string);
extern bool send_command(byte *command, int length);

enum DashboardWorkflowType : uint8_t {
  DASHBOARD_WORKFLOW_NONE = 0,
  DASHBOARD_WORKFLOW_DHW,
  DASHBOARD_WORKFLOW_STERILIZATION
};

enum DashboardWorkflowStage : uint8_t {
  DASHBOARD_WORKFLOW_IDLE = 0,
  DASHBOARD_WORKFLOW_WAIT_FORCE,
  DASHBOARD_WORKFLOW_ACTIVE,
  DASHBOARD_WORKFLOW_WAIT_RESTORE
};

struct DashboardWorkflowState {
  DashboardWorkflowType type;
  DashboardWorkflowStage stage;
  int8_t previousMode;
  bool modeChanged;
  bool observedActive;
  unsigned long nextActionAt;
  unsigned long forceSentAt;
};

static DashboardWorkflowState dashboardWorkflow = {
  DASHBOARD_WORKFLOW_NONE,
  DASHBOARD_WORKFLOW_IDLE,
  -1,
  false,
  false,
  0,
  0
};
static char dashboardWorkflowMessage[128] = "Ready";

static SchedulerManager schedulerManager;
static SmartDhwController smartDhwController;
static ExternalSensorRegistry externalSensors;
static time_t lastNtpSyncEpoch = 0;

void customFeaturesAppendExternalSensorDiagnostics(JsonArray array) {
  externalSensors.appendDiagnostics(array);
}

void customFeaturesReadExternalSensorHistory(float *values, bool *valid,
    size_t maxValues) {
  externalSensors.readHistory(values, valid, maxValues);
}

bool customFeaturesReadExternalElectricalPower(uint8_t sourceId, float *value,
    uint32_t *ageSeconds) {
  return externalSensors.readElectricalPower(sourceId, value, ageSeconds);
}

static bool schedulerReadTopic(uint8_t topic, float *value);
static bool schedulerReadValue(SchedulerConditionSource source, uint8_t sourceId,
  float *value, uint32_t *ageSeconds, char *detail, size_t detailSize);
static bool smartDhwReadTopic(uint8_t topic, float *value);
static SchedulerDispatchResult schedulerDispatchAction(SchedulerActionType action,
  int16_t value, char *detail, size_t detailSize);
static bool dispatchZone1HeatSemanticCommand(const char *commandName,
  const char *valueText, Zone1HeatRequestSemanticType expectedType,
  char *response, size_t responseSize);

static bool dashboardWorkflowTimeReached(unsigned long target) {
  return (long)(millis() - target) >= 0;
}

static bool dashboardWorkflowHasData() {
  return (actData[0] == 0x71) && (actData[1] == 0xC8) &&
    (actData[2] == 0x01) && (actData[3] == 0x10);
}

static int dashboardWorkflowTopicValue(uint8_t topic) {
  return getDataValue(actData, topic).toInt();
}

static const char *dashboardWorkflowTypeName() {
  switch (dashboardWorkflow.type) {
    case DASHBOARD_WORKFLOW_DHW: return "dhw";
    case DASHBOARD_WORKFLOW_STERILIZATION: return "sterilization";
    default: return "none";
  }
}

static const char *dashboardWorkflowStageName() {
  switch (dashboardWorkflow.stage) {
    case DASHBOARD_WORKFLOW_WAIT_FORCE: return "preparing";
    case DASHBOARD_WORKFLOW_ACTIVE: return "active";
    case DASHBOARD_WORKFLOW_WAIT_RESTORE: return "stopping";
    default: return "idle";
  }
}

static void dashboardWorkflowSetMessage(const char *message) {
  snprintf(dashboardWorkflowMessage, sizeof(dashboardWorkflowMessage), "%s", message);
  log_message(dashboardWorkflowMessage);
}

static void dashboardWorkflowReset(const char *message) {
  dashboardWorkflow.type = DASHBOARD_WORKFLOW_NONE;
  dashboardWorkflow.stage = DASHBOARD_WORKFLOW_IDLE;
  dashboardWorkflow.previousMode = -1;
  dashboardWorkflow.modeChanged = false;
  dashboardWorkflow.observedActive = false;
  dashboardWorkflow.nextActionAt = 0;
  dashboardWorkflow.forceSentAt = 0;
  dashboardWorkflowSetMessage(message);
}

static bool dashboardWorkflowSendCommand(bool operationModeCommand, int value) {
  unsigned char cmd[256] = { 0 };
  char valueString[12] = { 0 };
  char commandLog[256] = { 0 };
  snprintf(valueString, sizeof(valueString), "%d", value);

  unsigned int len;
  if (operationModeCommand) {
    len = set_operation_mode(valueString, cmd, commandLog);
  } else if (dashboardWorkflow.type == DASHBOARD_WORKFLOW_DHW) {
    len = set_force_DHW(valueString, cmd, commandLog);
  } else if (dashboardWorkflow.type == DASHBOARD_WORKFLOW_STERILIZATION) {
    len = set_force_sterilization(valueString, cmd, commandLog);
  } else {
    return false;
  }

  log_message(commandLog);
  return send_command(cmd, len);
}

static void dashboardWorkflowScheduleRestore(unsigned long delayMs, const char *message) {
  dashboardWorkflow.stage = DASHBOARD_WORKFLOW_WAIT_RESTORE;
  dashboardWorkflow.nextActionAt = millis() + delayMs;
  dashboardWorkflowSetMessage(message);
}

static bool dashboardWorkflowStart(DashboardWorkflowType type, char *response, size_t responseSize) {
  if (heishamonSettings.listenonly) {
    snprintf(response, responseSize, "Dashboard workflow unavailable in listen-only mode");
    return false;
  }
  if (!dashboardWorkflowHasData()) {
    snprintf(response, responseSize, "No valid heat pump data available yet");
    return false;
  }
  if (dashboardWorkflow.type != DASHBOARD_WORKFLOW_NONE) {
    snprintf(response, responseSize, "Another dashboard workflow is already running");
    return false;
  }

  if (type == DASHBOARD_WORKFLOW_DHW &&
      dashboardWorkflowTopicValue(10) >= heishamonSettings.wpDhwBlockAbove) {
    snprintf(response, responseSize, "DHW temperature is at or above the configured Force DHW limit");
    return false;
  }

  uint8_t stateTopic = (type == DASHBOARD_WORKFLOW_DHW) ? 2 : 69;
  if (dashboardWorkflowTopicValue(stateTopic) != 0) {
    snprintf(response, responseSize, "The requested function is already active outside this dashboard workflow");
    return false;
  }

  int currentMode = dashboardWorkflowTopicValue(4);
  if (currentMode == 7) currentMode = 2;
  if (currentMode == 8) currentMode = 6;
  if ((currentMode < 0) || (currentMode > 6)) {
    snprintf(response, responseSize, "Current operating mode is unknown");
    return false;
  }

  dashboardWorkflow.type = type;
  dashboardWorkflow.stage = DASHBOARD_WORKFLOW_WAIT_FORCE;
  dashboardWorkflow.previousMode = currentMode;
  dashboardWorkflow.modeChanged = (currentMode != 3);
  dashboardWorkflow.observedActive = false;
  dashboardWorkflow.forceSentAt = 0;

  if (dashboardWorkflow.modeChanged && !dashboardWorkflowSendCommand(true, 3)) {
    dashboardWorkflowReset("Could not switch to DHW-only mode");
    snprintf(response, responseSize, "Could not switch to DHW-only mode");
    return false;
  }

  dashboardWorkflow.nextActionAt = millis() + (dashboardWorkflow.modeChanged ? 2500UL : 250UL);
  dashboardWorkflowSetMessage(type == DASHBOARD_WORKFLOW_DHW ?
    "Preparing forced DHW cycle" : "Preparing forced sterilization cycle");
  snprintf(response, responseSize, "Workflow started");
  return true;
}

static bool dashboardWorkflowCancel(DashboardWorkflowType type, char *response, size_t responseSize) {
  if (dashboardWorkflow.type != type) {
    snprintf(response, responseSize, "This workflow is not running");
    return false;
  }
  if (dashboardWorkflow.stage == DASHBOARD_WORKFLOW_WAIT_RESTORE) {
    snprintf(response, responseSize, "Workflow is already stopping");
    return true;
  }

  if (dashboardWorkflow.stage == DASHBOARD_WORKFLOW_ACTIVE) {
    dashboardWorkflowSendCommand(false, 0);
  }
  dashboardWorkflowScheduleRestore(10000UL, type == DASHBOARD_WORKFLOW_DHW ?
    "Forced DHW cancelled; restoring operating mode" :
    "Forced sterilization cancelled; restoring operating mode");
  snprintf(response, responseSize, "Workflow cancellation requested");
  return true;
}

static bool dashboardWorkflowRequest(const char *action, char *response, size_t responseSize) {
  if (strcmp(action, "start_dhw") == 0) {
    return dashboardWorkflowStart(DASHBOARD_WORKFLOW_DHW, response, responseSize);
  }
  if (strcmp(action, "cancel_dhw") == 0) {
    return dashboardWorkflowCancel(DASHBOARD_WORKFLOW_DHW, response, responseSize);
  }
  if (strcmp(action, "start_sterilization") == 0) {
    return dashboardWorkflowStart(DASHBOARD_WORKFLOW_STERILIZATION, response, responseSize);
  }
  if (strcmp(action, "cancel_sterilization") == 0) {
    return dashboardWorkflowCancel(DASHBOARD_WORKFLOW_STERILIZATION, response, responseSize);
  }
  snprintf(response, responseSize, "Unknown dashboard workflow action");
  return false;
}

static void processDashboardWorkflow() {
  if (dashboardWorkflow.type == DASHBOARD_WORKFLOW_NONE) return;

  if (dashboardWorkflow.stage == DASHBOARD_WORKFLOW_WAIT_FORCE &&
      dashboardWorkflowTimeReached(dashboardWorkflow.nextActionAt)) {
    if (dashboardWorkflowSendCommand(false, 1)) {
      dashboardWorkflow.stage = DASHBOARD_WORKFLOW_ACTIVE;
      dashboardWorkflow.forceSentAt = millis();
      dashboardWorkflowSetMessage(dashboardWorkflow.type == DASHBOARD_WORKFLOW_DHW ?
        "Forced DHW cycle active" : "Forced sterilization cycle active");
    } else {
      dashboardWorkflowScheduleRestore(1000UL, "Could not send force command; restoring operating mode");
    }
    return;
  }

  if (dashboardWorkflow.stage == DASHBOARD_WORKFLOW_ACTIVE && dashboardWorkflowHasData()) {
    uint8_t stateTopic = (dashboardWorkflow.type == DASHBOARD_WORKFLOW_DHW) ? 2 : 69;
    int actualState = dashboardWorkflowTopicValue(stateTopic);
    if (actualState == 1) dashboardWorkflow.observedActive = true;

    if (!dashboardWorkflow.observedActive &&
        ((unsigned long)(millis() - dashboardWorkflow.forceSentAt) > 60000UL)) {
      dashboardWorkflowScheduleRestore(1000UL, "Force state was not confirmed; restoring operating mode");
      return;
    }

    int dhwActual = dashboardWorkflowTopicValue(10);
    int dhwTarget = dashboardWorkflowTopicValue(9);
    if (dashboardWorkflow.observedActive && actualState == 0 && (dhwActual + 2 >= dhwTarget)) {
      if (dashboardWorkflow.type == DASHBOARD_WORKFLOW_DHW) {
        dashboardWorkflowScheduleRestore(15UL * 60UL * 1000UL,
          "Forced DHW complete; operating mode will be restored in 15 minutes");
      } else {
        dashboardWorkflowScheduleRestore(10000UL,
          "Sterilization complete; restoring operating mode");
      }
    }
    return;
  }

  if (dashboardWorkflow.stage == DASHBOARD_WORKFLOW_WAIT_RESTORE &&
      dashboardWorkflowTimeReached(dashboardWorkflow.nextActionAt)) {
    int previousMode = dashboardWorkflow.previousMode;
    if (dashboardWorkflow.modeChanged && previousMode >= 0 && previousMode <= 6) {
      if (!dashboardWorkflowSendCommand(true, previousMode)) {
        dashboardWorkflow.nextActionAt = millis() + 5000UL;
        dashboardWorkflowSetMessage("Operating mode restore failed; retrying");
        return;
      }
    }
    dashboardWorkflowReset("Dashboard workflow finished");
  }
}

static bool schedulerReadTopic(uint8_t topic, float *value) {
  if (value == nullptr || topic >= NUMBER_OF_TOPICS) return false;
  if (actData[0] != 0x71 || actData[1] != 0xC8 ||
      actData[2] != 0x01 || actData[3] != 0x10) return false;
  unsigned long maximumAge = (unsigned long)heishamonSettings.waitTime * 4000UL;
  if (maximumAge < 60000UL) maximumAge = 60000UL;
  if (lastHeatpumpDataAt == 0 ||
      (unsigned long)(millis() - lastHeatpumpDataAt) > maximumAge) return false;

  String topicValue = getDataValue(actData, topic);
  if (topicValue.length() == 0) return false;
  return schedulerParseFiniteNumber(topicValue.c_str(), *value);
}

static bool smartDhwReadTopic(uint8_t topic, float *value) {
  return schedulerReadTopic(topic, value);
}

static bool schedulerReadValue(SchedulerConditionSource source, uint8_t sourceId,
    float *value, uint32_t *ageSeconds, char *detail, size_t detailSize) {
  if (source == SCHEDULER_SOURCE_MQTT) {
    return externalSensors.read(sourceId, value, ageSeconds, detail, detailSize);
  }
  bool valid = schedulerReadTopic(sourceId, value);
  if (ageSeconds != nullptr) *ageSeconds = valid ? 0 : UINT32_MAX;
  if (!valid && detail != nullptr && detailSize > 0) {
    snprintf(detail, detailSize, "Panasonic value TOP%u unavailable", sourceId);
  }
  return valid;
}

static SchedulerDispatchResult schedulerDispatchAction(SchedulerActionType action,
    int16_t value, char *detail, size_t detailSize) {
  if (heishamonSettings.listenonly) {
    snprintf(detail, detailSize, "Listen-only mode");
    return SCHEDULER_DISPATCH_FAILED;
  }

  float current = 0;
  uint8_t stateTopic = 255;
  int desired = value;
  switch (action) {
    case SCHEDULER_ACTION_HEATPUMP_ON: stateTopic = 0; desired = 1; break;
    case SCHEDULER_ACTION_HEATPUMP_OFF: stateTopic = 0; desired = 0; break;
    case SCHEDULER_ACTION_SET_OPERATION_MODE: stateTopic = 4; break;
    case SCHEDULER_ACTION_SET_DHW_TARGET: stateTopic = 9; break;
    case SCHEDULER_ACTION_SET_QUIET_MODE: stateTopic = 18; break;
    case SCHEDULER_ACTION_SET_Z1_REQUEST:
      snprintf(detail, detailSize, "Legacy ambiguous Zone 1 request action is disabled; edit the schedule");
      return SCHEDULER_DISPATCH_FAILED;
    default: break;
  }

  if (action == SCHEDULER_ACTION_SET_HEAT_CURVE_SHIFT) {
    char response[160] = {0};
    bool accepted = heatingCurveShiftSet(desired, response, sizeof(response));
    strlcpy(detail, response, detailSize);
    return accepted ? SCHEDULER_DISPATCH_EXECUTED : SCHEDULER_DISPATCH_FAILED;
  }

  if (action == SCHEDULER_ACTION_SET_Z1_HEATING_WATER_TARGET ||
      action == SCHEDULER_ACTION_SET_Z1_ROOM_TARGET) {
    Zone1HeatRequestSemanticType expectedType = ZONE1_HEAT_SEMANTIC_UNKNOWN;
    if (action == SCHEDULER_ACTION_SET_Z1_HEATING_WATER_TARGET) expectedType = ZONE1_HEATING_WATER_TARGET;
    else expectedType = ZONE1_ROOM_TARGET;
    char valueString[16] = {0};
    snprintf(valueString, sizeof(valueString), "%d", desired);
    bool accepted = dispatchZone1HeatSemanticCommand(
      SchedulerManager::actionName(action), valueString, expectedType,
      detail, detailSize);
  return accepted ? (strstr(detail, "already has requested value") != nullptr ?
      SCHEDULER_DISPATCH_NO_CHANGE : SCHEDULER_DISPATCH_EXECUTED) :
      SCHEDULER_DISPATCH_FAILED;
  }

  if (stateTopic != 255 && schedulerReadTopic(stateTopic, &current)) {
    int normalizedCurrent = (int)lroundf(current);
    if (action == SCHEDULER_ACTION_SET_OPERATION_MODE) {
      if (normalizedCurrent == 7) normalizedCurrent = 2;
      if (normalizedCurrent == 8) normalizedCurrent = 6;
    }
    if (normalizedCurrent == desired) {
      snprintf(detail, detailSize, "%s already has requested value %d",
        SchedulerManager::actionName(action), desired);
      return SCHEDULER_DISPATCH_NO_CHANGE;
    }
  }

  if (action == SCHEDULER_ACTION_FORCE_DHW) {
    if (dashboardWorkflow.type != DASHBOARD_WORKFLOW_NONE) {
      snprintf(detail, detailSize, "Dashboard workflow is busy");
      return SCHEDULER_DISPATCH_BUSY;
    }
    char workflowResponse[128] = {0};
    if (!dashboardWorkflowStart(DASHBOARD_WORKFLOW_DHW, workflowResponse, sizeof(workflowResponse))) {
      strlcpy(detail, workflowResponse, detailSize);
      return SCHEDULER_DISPATCH_FAILED;
    }
    strlcpy(detail, workflowResponse, detailSize);
    return SCHEDULER_DISPATCH_EXECUTED;
  }

  if (dashboardWorkflow.type != DASHBOARD_WORKFLOW_NONE) {
    snprintf(detail, detailSize, "Dashboard workflow is busy");
    return SCHEDULER_DISPATCH_BUSY;
  }

  unsigned char command[256] = {0};
  char commandLog[256] = {0};
  char valueString[16];
  snprintf(valueString, sizeof(valueString), "%d", desired);
  unsigned int length = 0;
  switch (action) {
    case SCHEDULER_ACTION_HEATPUMP_ON:
    case SCHEDULER_ACTION_HEATPUMP_OFF:
      length = set_heatpump_state(valueString, command, commandLog); break;
    case SCHEDULER_ACTION_SET_OPERATION_MODE:
      length = set_operation_mode(valueString, command, commandLog); break;
    case SCHEDULER_ACTION_SET_DHW_TARGET:
      length = set_DHW_temp(valueString, command, commandLog); break;
    case SCHEDULER_ACTION_SET_QUIET_MODE:
      length = set_quiet_mode(valueString, command, commandLog); break;
    default:
      snprintf(detail, detailSize, "Unsupported action");
      return SCHEDULER_DISPATCH_FAILED;
  }

  if (length == 0 || !send_command(command, length)) {
    snprintf(detail, detailSize, "Panasonic command queue rejected action");
    return SCHEDULER_DISPATCH_FAILED;
  }
  strlcpy(detail, commandLog, detailSize);
  return SCHEDULER_DISPATCH_EXECUTED;
}

static int handleDashboardWorkflowStatus(struct webserver_t *client) {
  if (client->content == 0) {
    char response[320];
    snprintf(response, sizeof(response),
      "{\"type\":\"%s\",\"stage\":\"%s\",\"previousMode\":%d,\"message\":\"%s\"}",
      dashboardWorkflowTypeName(), dashboardWorkflowStageName(),
      dashboardWorkflow.previousMode, dashboardWorkflowMessage);
    webserver_send(client, 200, (char *)"application/json", strlen(response));
    webserver_send_content(client, response, strlen(response));
  }
  return 0;
}

static int handleSchedulerStatus(struct webserver_t *client) {
  if (client->content != 0) return 0;
  JsonDocument document;
  schedulerManager.toJson(document);
  zone1HeatRequestSemanticToJson(document["zone1HeatRequest"].to<JsonObject>(),
    actData, heishamonSettings.wpHeatMin, heishamonSettings.wpHeatMax);
  heatingCurveShiftToJson(document["heatingCurveShift"].to<JsonObject>());
  document["ntpSynchronized"] = lastNtpSyncEpoch > 0;
  char lastSync[20] = "Never";
  if (lastNtpSyncEpoch > 0) {
    struct tm local;
    if (localtime_r(&lastNtpSyncEpoch, &local) != nullptr) {
      strftime(lastSync, sizeof(lastSync), "%Y-%m-%d %H:%M:%S", &local);
    }
  }
  document["lastNtpSync"] = lastSync;
  JsonArray sensors = document["externalSensors"].to<JsonArray>();
  externalSensors.appendConditionSources(sensors);
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 20);
    webserver_send_content_P(client, PSTR("Scheduler unavailable"), 20);
    return 0;
  }
  serializeJson(document, response, length + 1);
  webserver_send(client, 200, (char *)"application/json", length);
  webserver_send_content(client, response, length);
  free(response);
  return 0;
}

static int handleSmartDhwStatus(struct webserver_t *client) {
  if (client->content != 0) return 0;
  JsonDocument document;
  smartDhwController.toJson(document);
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 21);
    webserver_send_content_P(client, PSTR("Smart DHW unavailable"), 21);
    return 0;
  }
  serializeJson(document, response, length + 1);
  webserver_send(client, 200, (char *)"application/json", length);
  webserver_send_content(client, response, length);
  free(response);
  return 0;
}

static int handleExternalSensorsStatus(struct webserver_t *client) {
  if (client->content != 0) return 0;
  JsonDocument document;
  externalSensors.toJson(document);
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 28);
    webserver_send_content_P(client, PSTR("External sensors unavailable"), 28);
    return 0;
  }
  serializeJson(document, response, length + 1);
  webserver_send(client, 200, (char *)"application/json", length);
  webserver_send_content(client, response, length);
  free(response);
  return 0;
}

static void appendCustomResponse(struct webserver_t *client, const char *message) {
  size_t oldLength = client->userdata == nullptr ? 0 : strlen((char *)client->userdata);
  size_t addLength = strlen(message);
  char *response = (char *)realloc(client->userdata, oldLength + addLength + 2);
  if (response == nullptr) {
    log_message((char *)"Out of memory while building custom response");
    ESP.restart();
    return;
  }
  client->userdata = response;
  memcpy(response + oldLength, message, addLength);
  response[oldLength + addLength] = '\n';
  response[oldLength + addLength + 1] = '\0';
}

static void handleSchedulerArgument(struct webserver_t *client, struct arguments_t *args) {
  char name[24] = {0};
  snprintf(name, sizeof(name), "%s", (char *)args->name);
  char value[args->len + 1];
  snprintf(value, sizeof(value), "%.*s", args->len, args->value);
  char response[160] = {0};
  bool accepted = false;

  if (strcmp(name, "save") == 0) {
    JsonDocument document;
    DeserializationError error = deserializeJson(document, value);
    if (error || !document.is<JsonObject>()) {
      snprintf(response, sizeof(response), "Invalid scheduler JSON");
    } else {
      accepted = schedulerManager.upsert(document.as<JsonObjectConst>(), response, sizeof(response));
    }
  } else if (strcmp(name, "delete") == 0 || strcmp(name, "run") == 0) {
    char *end = nullptr;
    long id = strtol(value, &end, 10);
    if (end == value || *end != '\0' || id < 1 || id > 255) {
      snprintf(response, sizeof(response), "Invalid schedule ID");
    } else if (strcmp(name, "delete") == 0) {
      accepted = schedulerManager.remove((uint8_t)id, response, sizeof(response));
    } else {
      accepted = schedulerManager.runNow((uint8_t)id, response, sizeof(response));
    }
  } else if (strcmp(name, "enabled") == 0) {
    if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
      snprintf(response, sizeof(response), "Scheduler enabled must be 0 or 1");
    } else {
      accepted = schedulerManager.setEnabled(strcmp(value, "1") == 0, response, sizeof(response));
    }
  } else {
    snprintf(response, sizeof(response), "Unknown scheduler command");
  }

  char result[192];
  snprintf(result, sizeof(result), "%s: %s", accepted ? "OK" : "ERROR", response);
  appendCustomResponse(client, result);
  if (accepted) diagnosticsHistoryRecordEvent(HISTORY_EVENT_SCHEDULER,
    "Scheduler command accepted");
  log_message(result);
}

static void handleSmartDhwArgument(struct webserver_t *client, struct arguments_t *args) {
  char name[16] = {0};
  snprintf(name, sizeof(name), "%s", (char *)args->name);
  char value[args->len + 1];
  snprintf(value, sizeof(value), "%.*s", args->len, args->value);
  char response[192] = {0};
  bool accepted = false;

  if (strcmp(name, "save") == 0) {
    JsonDocument document;
    DeserializationError error = deserializeJson(document, value);
    if (error || !document.is<JsonObject>()) {
      snprintf(response, sizeof(response), "Invalid Smart DHW JSON");
    } else {
      accepted = smartDhwController.update(document.as<JsonObjectConst>(),
        response, sizeof(response));
    }
  } else if (strcmp(name, "test") == 0) {
    if (strcmp(value, "evening") == 0) {
      accepted = smartDhwController.testDecision(SMART_DHW_SLOT_EVENING,
        response, sizeof(response));
    } else if (strcmp(value, "morning") == 0) {
      accepted = smartDhwController.testDecision(SMART_DHW_SLOT_MORNING,
        response, sizeof(response));
    } else {
      snprintf(response, sizeof(response), "Test must be evening or morning");
    }
  } else {
    snprintf(response, sizeof(response), "Unknown Smart DHW command");
  }

  char result[224];
  snprintf(result, sizeof(result), "%s: %s", accepted ? "OK" : "ERROR", response);
  appendCustomResponse(client, result);
  if (accepted) diagnosticsHistoryRecordEvent(HISTORY_EVENT_SMART_DHW,
    "Smart DHW command accepted");
  log_message(result);
}

static void handleExternalSensorsArgument(struct webserver_t *client, struct arguments_t *args) {
  char name[16] = {0};
  snprintf(name, sizeof(name), "%s", (char *)args->name);
  char value[args->len + 1];
  snprintf(value, sizeof(value), "%.*s", args->len, args->value);
  char response[192] = {0};
  bool accepted = false;
  if (strcmp(name, "save") == 0) {
    JsonDocument document;
    DeserializationError error = deserializeJson(document, value);
    if (error || !document.is<JsonObject>()) snprintf(response, sizeof(response), "Invalid external sensor JSON");
    else accepted = externalSensors.upsert(document.as<JsonObjectConst>(), response, sizeof(response));
  } else if (strcmp(name, "delete") == 0) {
    char *end = nullptr;
    long id = strtol(value, &end, 10);
    if (end == value || *end != '\0' || id < 1 || id > 255) snprintf(response, sizeof(response), "Invalid sensor ID");
    else accepted = externalSensors.remove((uint8_t)id, response, sizeof(response));
  } else {
    snprintf(response, sizeof(response), "Unknown external sensor command");
  }
  char result[224];
  snprintf(result, sizeof(result), "%s: %s", accepted ? "OK" : "ERROR", response);
  appendCustomResponse(client, result);
  log_message(result);
}

static int handleWpSettingsConfigStatus(struct webserver_t *client) {
  if (client->content == 0) {
    char response[128];
    snprintf(response, sizeof(response),
      "{\"heatMin\":%d,\"heatMax\":%d,\"dhwBlockAbove\":%d}",
      heishamonSettings.wpHeatMin, heishamonSettings.wpHeatMax,
      heishamonSettings.wpDhwBlockAbove);
    webserver_send(client, 200, (char *)"application/json", strlen(response));
    webserver_send_content(client, response, strlen(response));
  }
  return 0;
}

static int handleZone1HeatSemanticStatus(struct webserver_t *client) {
  if (client->content != 0) return 0;
  JsonDocument document;
  zone1HeatRequestSemanticToJson(document.to<JsonObject>(), actData,
    heishamonSettings.wpHeatMin, heishamonSettings.wpHeatMax);
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 26);
    webserver_send_content_P(client, PSTR("Semantic state unavailable"), 26);
    return 0;
  }
  serializeJson(document, response, length + 1);
  webserver_send(client, 200, (char *)"application/json", length);
  webserver_send_content(client, response, length);
  free(response);
  return 0;
}

static int handleHeatingCurveShiftStatus(struct webserver_t *client) {
  if (client->content != 0) return 0;
  JsonDocument document;
  heatingCurveShiftToJson(document.to<JsonObject>());
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 23);
    webserver_send_content_P(client, PSTR("Curve shift unavailable"), 23);
    return 0;
  }
  serializeJson(document, response, length + 1);
  webserver_send(client, 200, (char *)"application/json", length);
  webserver_send_content(client, response, length);
  free(response);
  return 0;
}

static bool dispatchZone1HeatSemanticCommand(const char *commandName,
    const char *valueText, Zone1HeatRequestSemanticType expectedType,
    char *response, size_t responseSize) {
  if (heishamonSettings.listenonly) {
    snprintf(response, responseSize, "Listen-only mode");
    return false;
  }

  Zone1HeatRequestSemantic semantic;
  if (!resolveZone1HeatRequestSemantic(actData, heishamonSettings.wpHeatMin,
      heishamonSettings.wpHeatMax, &semantic)) {
    snprintf(response, responseSize, "Zone 1 heat request semantics unavailable");
    return false;
  }
  if (semantic.type != expectedType) {
    snprintf(response, responseSize, "Current Zone 1 mode is %s, not %s",
      semantic.name, zone1HeatRequestSemanticName(expectedType));
    return false;
  }

  char *end = nullptr;
  long parsed = strtol(valueText, &end, 10);
  if (end == valueText || *end != '\0' || parsed < INT16_MIN || parsed > INT16_MAX ||
      !zone1HeatRequestValueValid(semantic, (float)parsed)) {
    snprintf(response, responseSize, "%s value must be an integer between %d and %d %s",
      semantic.label, semantic.minValue, semantic.maxValue, semantic.unit);
    return false;
  }

  float current = 0;
  if (readZone1HeatRequestRaw(actData, &current) &&
      zone1HeatRequestValueValid(semantic, current) && lroundf(current) == parsed) {
    snprintf(response, responseSize, "%s already has requested value %ld %s",
      semantic.label, parsed, semantic.unit);
    return true;
  }

  unsigned char command[256] = {0};
  char commandLog[256] = {0};
  char valueBuffer[16] = {0};
  snprintf(valueBuffer, sizeof(valueBuffer), "%ld", parsed);
  unsigned int length = set_z1_heat_request_temperature(valueBuffer, command, commandLog);
  if (length == 0 || !send_command(command, length)) {
    snprintf(response, responseSize, "%s command queue rejected", commandName);
    return false;
  }
  snprintf(response, responseSize, "%s: %s", commandName, commandLog);
  log_message(commandLog);
  return true;
}

static bool updateWpSettingsConfig(const char *name, const char *value,
    char *response, size_t responseSize) {
  char *end = nullptr;
  long parsed = strtol(value, &end, 10);
  if ((end == value) || (*end != '\0')) {
    snprintf(response, responseSize, "Invalid numeric WP setting");
    return false;
  }

  if (strcmp(name, "WpHeatMin") == 0) {
    if (parsed < 20 || parsed > heishamonSettings.wpHeatMax) {
      snprintf(response, responseSize, "Heat minimum must be between 20 and the configured maximum");
      return false;
    }
    heishamonSettings.wpHeatMin = parsed;
  } else if (strcmp(name, "WpHeatMax") == 0) {
    if (parsed < heishamonSettings.wpHeatMin || parsed > 100) {
      snprintf(response, responseSize, "Heat maximum must be between the configured minimum and 100");
      return false;
    }
    heishamonSettings.wpHeatMax = parsed;
  } else if (strcmp(name, "WpDhwBlockAbove") == 0) {
    if (parsed < 40 || parsed > 100) {
      snprintf(response, responseSize, "DHW limit must be between 40 and 100");
      return false;
    }
    heishamonSettings.wpDhwBlockAbove = parsed;
  } else {
    return false;
  }

  JsonDocument jsonDoc;
  settingsToJson(jsonDoc, &heishamonSettings);
  saveJsonToFile(jsonDoc, "/config.json");
  snprintf(response, responseSize, "WP setting saved");
  return true;
}

bool customFeaturesHandleUri(struct webserver_t *client, const char *uri) {
  if (diagnosticsHistoryHandleUri(client, uri)) return true;
  if (strcmp(uri, "/dashboardworkflow") == 0) client->route = 15;
  else if (strcmp(uri, "/wpsettingsconfig") == 0) client->route = 16;
  else if (strcmp(uri, "/scheduler") == 0) client->route = 12;
  else if (strcmp(uri, "/schedulerapi") == 0) client->route = 13;
  else if (strcmp(uri, "/schedulercommand") == 0) client->route = 14;
  else if (strcmp(uri, "/smartdhw") == 0) client->route = 17;
  else if (strcmp(uri, "/smartdhwapi") == 0) client->route = 18;
  else if (strcmp(uri, "/smartdhwcommand") == 0) client->route = 19;
  else if (strcmp(uri, "/externalsensors") == 0) client->route = 24;
  else if (strcmp(uri, "/externalsensorsapi") == 0) client->route = 25;
  else if (strcmp(uri, "/externalsensorscommand") == 0) client->route = 26;
  else if (strcmp(uri, "/zone1heatsemantic") == 0) client->route = 36;
  else if (strcmp(uri, "/heatingcurveshift") == 0) client->route = 37;
  else return false;

  if (client->route == 14 || client->route == 19 || client->route == 26) {
    client->userdata = malloc(1);
    if (client->userdata == nullptr) {
      log_message((char *)"Out of memory while creating custom request");
      ESP.restart();
      return true;
    }
    ((char *)client->userdata)[0] = '\0';
  }
  return true;
}

bool customFeaturesHandleArgs(struct webserver_t *client, struct arguments_t *args) {
  if (diagnosticsHistoryHandleArgs(client, args)) return true;
  if (client->route == 14) {
    handleSchedulerArgument(client, args);
    return true;
  }
  if (client->route == 19) {
    handleSmartDhwArgument(client, args);
    return true;
  }
  if (client->route == 26) {
    handleExternalSensorsArgument(client, args);
    return true;
  }
  return false;
}

bool customFeaturesHandleCommandArgument(struct webserver_t *client, struct arguments_t *args) {
  char value[args->len + 1];
  snprintf(value, sizeof(value), "%.*s", args->len, args->value);

  if (strcmp((char *)args->name, "DashboardWorkflow") == 0) {
    char response[160] = {0};
    dashboardWorkflowRequest(value, response, sizeof(response));
    appendCustomResponse(client, response);
    log_message(response);
    return true;
  }

  if (strcmp((char *)args->name, "SetHeatingCurveShift") == 0 ||
      strcmp((char *)args->name, "SetZ1HeatCurveBaseHigh") == 0 ||
      strcmp((char *)args->name, "SetZ1HeatCurveBaseLow") == 0) {
    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    char response[192] = {0};
    bool accepted = end != value && *end == '\0' && parsed >= -32768 && parsed <= 32767;
    if (accepted && strcmp((char *)args->name, "SetHeatingCurveShift") == 0) {
      accepted = heatingCurveShiftSet((int)parsed, response, sizeof(response));
    } else if (accepted) {
      accepted = heatingCurveShiftSetBase(
        strcmp((char *)args->name, "SetZ1HeatCurveBaseHigh") == 0,
        (int)parsed, response, sizeof(response));
    }
    if (!accepted && response[0] == '\0') snprintf(response, sizeof(response), "Invalid curve value");
    appendCustomResponse(client, response);
    log_message(response);
    return true;
  }

  Zone1HeatRequestSemanticType expectedType = ZONE1_HEAT_SEMANTIC_UNKNOWN;
  if (strcmp((char *)args->name, "SetZ1HeatingWaterTarget") == 0) {
    expectedType = ZONE1_HEATING_WATER_TARGET;
  } else if (strcmp((char *)args->name, "SetZ1RoomTarget") == 0) {
    expectedType = ZONE1_ROOM_TARGET;
  }
  if (expectedType != ZONE1_HEAT_SEMANTIC_UNKNOWN) {
    char response[192] = {0};
    bool accepted = dispatchZone1HeatSemanticCommand((char *)args->name,
      value, expectedType, response, sizeof(response));
    appendCustomResponse(client, response);
    log_message(response);
    return true;
  }

  if (strcmp((char *)args->name, "WpHeatMin") == 0 ||
      strcmp((char *)args->name, "WpHeatMax") == 0 ||
      strcmp((char *)args->name, "WpDhwBlockAbove") == 0) {
    char response[160] = {0};
    updateWpSettingsConfig((char *)args->name, value, response, sizeof(response));
    appendCustomResponse(client, response);
    log_message(response);
    return true;
  }
  return false;
}

bool customFeaturesHandleWrite(struct webserver_t *client) {
  if (diagnosticsHistoryHandleWrite(client)) return true;
  switch (client->route) {
    case 12: handleScheduler(client); return true;
    case 13: handleSchedulerStatus(client); return true;
    case 14:
    case 19:
      if (client->content == 0) {
        char *response = (char *)client->userdata;
        size_t length = response == nullptr ? 0 : strlen(response);
        webserver_send(client, 200, (char *)"text/plain", length);
        if (length > 0) webserver_send_content(client, response, length);
        free(response);
        client->userdata = nullptr;
      }
      return true;
    case 15: handleDashboardWorkflowStatus(client); return true;
    case 16: handleWpSettingsConfigStatus(client); return true;
    case 17: handleSmartDhw(client); return true;
    case 18: handleSmartDhwStatus(client); return true;
    case 24: handleExternalSensors(client); return true;
    case 25: handleExternalSensorsStatus(client); return true;
    case 26:
      if (client->content == 0) {
        char *response = (char *)client->userdata;
        size_t length = response == nullptr ? 0 : strlen(response);
        webserver_send(client, 200, (char *)"text/plain", length);
        if (length > 0) webserver_send_content(client, response, length);
        free(response);
        client->userdata = nullptr;
      }
      return true;
    case 36: handleZone1HeatSemanticStatus(client); return true;
    case 37: handleHeatingCurveShiftStatus(client); return true;
    default: return false;
  }
}

bool customFeaturesHandleClose(struct webserver_t *client) {
  return diagnosticsHistoryHandleClose(client);
}

void customFeaturesBegin() {
  externalSensors.begin(log_message);
  diagnosticsHistoryBegin();
  log_message((char *)"Loading local scheduler...");
  schedulerManager.begin(schedulerReadValue, schedulerDispatchAction, log_message);
  log_message((char *)"Loading Smart DHW...");
  smartDhwController.begin(&schedulerManager, smartDhwReadTopic, log_message);
}

bool customFeaturesHandleMqttMessage(const char *topic, const char *mqttBase,
    const uint8_t *payload, size_t length) {
  if (topic == nullptr || mqttBase == nullptr || payload == nullptr) return false;
  size_t baseLength = strlen(mqttBase);
  size_t topicLength = strlen(topic);
  if (topicLength <= baseLength || strncmp(topic, mqttBase, baseLength) != 0 ||
      topic[baseLength] != '/') return false;
  return externalSensors.handleMqttMessage(topic + baseLength + 1, payload, length);
}

void customFeaturesMqttConnected(PubSubClient &client, const char *mqttBase) {
  externalSensors.setMqttConnected(true);
  externalSensors.subscribe(client, mqttBase);
}

void customFeaturesMqttDisconnected() {
  externalSensors.setMqttConnected(false);
}

void customFeaturesLoop(PubSubClient &mqttClient, const char *mqttBase) {
  time_t currentTime = time(nullptr);
  struct tm currentLocal;
  if (currentTime > 0 && localtime_r(&currentTime, &currentLocal) != nullptr &&
      currentLocal.tm_year + 1900 >= 2024 && lastNtpSyncEpoch == 0) {
    lastNtpSyncEpoch = currentTime;
  }
  if (!mqttClient.connected()) externalSensors.setMqttConnected(false);
  else if (externalSensors.subscriptionsDirty()) {
    externalSensors.setMqttConnected(true);
    externalSensors.subscribe(mqttClient, mqttBase);
  }
  processDashboardWorkflow();
  heatingCurveShiftLoop();
  schedulerManager.loop();
  smartDhwController.loop();
  diagnosticsHistoryLoop();
}
