#include "external_sensors.h"

#include <LittleFS.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "scheduler_logic.h"

static const char *EXTERNAL_SENSOR_FILE = "/external_sensors.json";

ExternalSensorRegistry::ExternalSensorRegistry()
  : count_(0), mqttConnected_(false), subscriptionsDirty_(true), logger_(nullptr) {
  memset(sensors_, 0, sizeof(sensors_));
}

void ExternalSensorRegistry::begin(void (*logger)(char *message)) {
  logger_ = logger;
  if (!load()) log("[MQTT] no valid external sensor configuration loaded");
}

void ExternalSensorRegistry::log(const char *message) const {
  if (logger_ != nullptr) logger_((char *)message);
}

int8_t ExternalSensorRegistry::findIndex(uint8_t id) const {
  for (uint8_t i = 0; i < count_; i++) if (sensors_[i].id == id) return (int8_t)i;
  return -1;
}

uint8_t ExternalSensorRegistry::nextId() const {
  for (uint16_t id = 1; id <= 255; id++) if (findIndex((uint8_t)id) < 0) return (uint8_t)id;
  return 0;
}

bool ExternalSensorRegistry::validateTopic(const char *topic, char *message,
    size_t messageSize) const {
  if (topic == nullptr || topic[0] == '\0' || strlen(topic) >= EXTERNAL_SENSOR_TOPIC_LENGTH) {
    snprintf(message, messageSize, "MQTT topic is required and must be at most 96 characters");
    return false;
  }
  if (strchr(topic, '#') != nullptr || strchr(topic, '+') != nullptr ||
      strchr(topic, ' ') != nullptr || topic[0] == '/') {
    snprintf(message, messageSize, "MQTT topic must be an exact relative topic without wildcards");
    return false;
  }
  static const char *reserved[] = {
    "commands/", "main/", "extra/", "optional/", "1wire/", "s0/", "gpio/",
    "SendRawValue", "LWT", "ip"
  };
  for (const char *prefix : reserved) {
    if (strncmp(topic, prefix, strlen(prefix)) == 0) {
      snprintf(message, messageSize, "This topic belongs to HeishaMon and cannot be used as an external sensor");
      return false;
    }
  }
  return true;
}

bool ExternalSensorRegistry::validate(JsonObjectConst object, ExternalSensor &sensor,
    char *message, size_t messageSize) const {
  long id = object["id"] | 0L;
  long timeout = object["staleTimeoutSeconds"] | 0L;
  const char *name = object["name"] | "";
  const char *topic = object["mqttTopic"] | "";
  const char *unit = object["unit"] | "";
  if (id < 0 || id > 255 || strlen(name) == 0 || strlen(name) >= EXTERNAL_SENSOR_NAME_LENGTH ||
      strlen(unit) >= EXTERNAL_SENSOR_UNIT_LENGTH || timeout < (long)EXTERNAL_SENSOR_MIN_STALE_SECONDS ||
      timeout > (long)EXTERNAL_SENSOR_MAX_STALE_SECONDS) {
    snprintf(message, messageSize, "Invalid sensor fields or stale timeout");
    return false;
  }
  if (!validateTopic(topic, message, messageSize)) return false;
  memset(&sensor, 0, sizeof(sensor));
  sensor.id = (uint8_t)id;
  sensor.enabled = object["enabled"] | true;
  strlcpy(sensor.name, name, sizeof(sensor.name));
  strlcpy(sensor.mqttTopic, topic, sizeof(sensor.mqttTopic));
  strlcpy(sensor.unit, unit, sizeof(sensor.unit));
  sensor.staleTimeoutSeconds = (uint32_t)timeout;
  return true;
}

bool ExternalSensorRegistry::upsert(JsonObjectConst object, char *message,
    size_t messageSize) {
  ExternalSensor sensor;
  if (!validate(object, sensor, message, messageSize)) return false;
  int8_t index = sensor.id == 0 ? -1 : findIndex(sensor.id);
  if (sensor.id != 0 && index < 0) {
    snprintf(message, messageSize, "Sensor ID not found");
    return false;
  }
  for (uint8_t i = 0; i < count_; i++) {
    if (i != (uint8_t)index && strcmp(sensors_[i].mqttTopic, sensor.mqttTopic) == 0) {
      snprintf(message, messageSize, "MQTT topic is already assigned to another sensor");
      return false;
    }
  }
  bool adding = index < 0;
  if (adding) {
    if (count_ >= EXTERNAL_SENSOR_MAX) {
      snprintf(message, messageSize, "Maximum of %u external sensors reached", EXTERNAL_SENSOR_MAX);
      return false;
    }
    sensor.id = nextId();
    if (sensor.id == 0) { snprintf(message, messageSize, "No sensor ID available"); return false; }
    sensors_[count_++] = sensor;
  } else {
    sensor.value = sensors_[index].value;
    sensor.valid = sensors_[index].valid;
    sensor.lastUpdate = sensors_[index].lastUpdate;
    sensors_[index] = sensor;
  }
  subscriptionsDirty_ = true;
  if (!save()) {
    if (adding) count_--;
    snprintf(message, messageSize, "Could not persist external sensor");
    return false;
  }
  snprintf(message, messageSize, "External sensor saved");
  return true;
}

bool ExternalSensorRegistry::remove(uint8_t id, char *message, size_t messageSize) {
  int8_t index = findIndex(id);
  if (index < 0) { snprintf(message, messageSize, "Sensor not found"); return false; }
  ExternalSensor removed = sensors_[index];
  for (uint8_t i = (uint8_t)index; i + 1 < count_; i++) sensors_[i] = sensors_[i + 1];
  count_--;
  subscriptionsDirty_ = true;
  if (!save()) {
    for (uint8_t i = count_; i > (uint8_t)index; i--) sensors_[i] = sensors_[i - 1];
    sensors_[index] = removed;
    count_++;
    snprintf(message, messageSize, "Could not persist removal");
    return false;
  }
  snprintf(message, messageSize, "External sensor removed");
  return true;
}

bool ExternalSensorRegistry::read(uint8_t id, float *value, uint32_t *ageSeconds,
    char *detail, size_t detailSize) const {
  if (value == nullptr || ageSeconds == nullptr) return false;
  int8_t index = findIndex(id);
  if (index < 0 || !sensors_[index].enabled) {
    snprintf(detail, detailSize, "External sensor %u unavailable", id);
    return false;
  }
  const ExternalSensor &sensor = sensors_[index];
  unsigned long ageMillis = sensor.lastUpdate == 0 ? ULONG_MAX : millis() - sensor.lastUpdate;
  *ageSeconds = ageMillis == ULONG_MAX ? UINT32_MAX : (uint32_t)(ageMillis / 1000UL);
  if (!sensor.valid || sensor.lastUpdate == 0 || *ageSeconds > sensor.staleTimeoutSeconds ||
      !isfinite(sensor.value)) {
    snprintf(detail, detailSize, "External sensor \"%s\" stale", sensor.name);
    return false;
  }
  *value = sensor.value;
  return true;
}

bool ExternalSensorRegistry::handleMqttMessage(const char *relativeTopic,
    const uint8_t *payload, size_t length) {
  if (relativeTopic == nullptr || payload == nullptr || length == 0 || length >= 128) return false;
  char valueString[128];
  memcpy(valueString, payload, length);
  valueString[length] = '\0';
  float parsed = 0;
  if (!schedulerParseFiniteNumber(valueString, parsed)) return false;
  for (uint8_t i = 0; i < count_; i++) {
    ExternalSensor &sensor = sensors_[i];
    if (sensor.enabled && strcmp(sensor.mqttTopic, relativeTopic) == 0) {
      sensor.value = parsed;
      sensor.valid = true;
      sensor.lastUpdate = millis();
      return true;
    }
  }
  return false;
}

void ExternalSensorRegistry::subscribe(PubSubClient &client, const char *mqttBase) {
  if (mqttBase == nullptr || mqttBase[0] == '\0') return;
  bool allSubscribed = true;
  for (uint8_t i = 0; i < count_; i++) {
    if (!sensors_[i].enabled) continue;
    char topic[256];
    int length = snprintf(topic, sizeof(topic), "%s/%s", mqttBase, sensors_[i].mqttTopic);
    if (length > 0 && (size_t)length < sizeof(topic) && !client.subscribe(topic)) {
      allSubscribed = false;
    }
  }
  subscriptionsDirty_ = !allSubscribed;
}

bool ExternalSensorRegistry::load() {
  count_ = 0;
  if (!LittleFS.begin() || !LittleFS.exists(EXTERNAL_SENSOR_FILE)) return true;
  File file = LittleFS.open(EXTERNAL_SENSOR_FILE, "r");
  if (!file) return false;
  JsonDocument document;
  DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error || (document["version"] | 0) != EXTERNAL_SENSOR_CONFIG_VERSION) return false;
  JsonArrayConst sensors = document["sensors"].as<JsonArrayConst>();
  for (JsonObjectConst object : sensors) {
    if (count_ >= EXTERNAL_SENSOR_MAX) break;
    ExternalSensor sensor;
    char message[96];
    if (!validate(object, sensor, message, sizeof(message)) || sensor.id == 0 || findIndex(sensor.id) >= 0) {
      char logMessage[160];
      snprintf(logMessage, sizeof(logMessage), "[MQTT] ignored invalid external sensor: %s", message);
      log(logMessage);
      continue;
    }
    sensors_[count_++] = sensor;
  }
  return true;
}

bool ExternalSensorRegistry::save() const {
  if (!LittleFS.begin()) return false;
  JsonDocument document;
  document["version"] = EXTERNAL_SENSOR_CONFIG_VERSION;
  JsonArray sensors = document["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < count_; i++) {
    const ExternalSensor &sensor = sensors_[i];
    JsonObject object = sensors.add<JsonObject>();
    object["id"] = sensor.id;
    object["enabled"] = sensor.enabled;
    object["name"] = sensor.name;
    object["mqttTopic"] = sensor.mqttTopic;
    object["unit"] = sensor.unit;
    object["staleTimeoutSeconds"] = sensor.staleTimeoutSeconds;
  }
  File file = LittleFS.open(EXTERNAL_SENSOR_FILE, "w");
  if (!file) return false;
  bool ok = serializeJson(document, file) > 0;
  file.close();
  return ok;
}

void ExternalSensorRegistry::appendConditionSources(JsonArray array) const {
  for (uint8_t i = 0; i < count_; i++) {
    JsonObject object = array.add<JsonObject>();
    object["id"] = sensors_[i].id;
    object["name"] = sensors_[i].name;
    object["unit"] = sensors_[i].unit;
    object["enabled"] = sensors_[i].enabled;
  }
}

void ExternalSensorRegistry::toJson(JsonDocument &document) const {
  document["version"] = EXTERNAL_SENSOR_CONFIG_VERSION;
  document["mqttConnected"] = mqttConnected_;
  document["count"] = count_;
  document["maxSensors"] = EXTERNAL_SENSOR_MAX;
  JsonArray sensors = document["sensors"].to<JsonArray>();
  for (uint8_t i = 0; i < count_; i++) {
    const ExternalSensor &sensor = sensors_[i];
    JsonObject object = sensors.add<JsonObject>();
    object["id"] = sensor.id;
    object["enabled"] = sensor.enabled;
    object["name"] = sensor.name;
    object["mqttTopic"] = sensor.mqttTopic;
    object["unit"] = sensor.unit;
    object["staleTimeoutSeconds"] = sensor.staleTimeoutSeconds;
    unsigned long age = sensor.lastUpdate == 0 ? ULONG_MAX : millis() - sensor.lastUpdate;
    uint32_t ageSeconds = age == ULONG_MAX ? UINT32_MAX : (uint32_t)(age / 1000UL);
    bool fresh = sensor.enabled && sensor.valid && sensor.lastUpdate != 0 &&
      ageSeconds <= sensor.staleTimeoutSeconds && isfinite(sensor.value);
    object["valid"] = fresh;
    if (fresh) object["value"] = sensor.value;
    if (ageSeconds == UINT32_MAX) object["ageSeconds"] = nullptr;
    else object["ageSeconds"] = ageSeconds;
    object["state"] = fresh ? "OK" : sensor.lastUpdate == 0 ? "NO DATA" : "STALE";
  }
}

void ExternalSensorRegistry::appendDiagnostics(JsonArray array) const {
  for (uint8_t i = 0; i < count_; i++) {
    const ExternalSensor &sensor = sensors_[i];
    unsigned long age = sensor.lastUpdate == 0 ? ULONG_MAX : millis() - sensor.lastUpdate;
    uint32_t ageSeconds = age == ULONG_MAX ? UINT32_MAX : (uint32_t)(age / 1000UL);
    bool fresh = sensor.enabled && sensor.valid && sensor.lastUpdate != 0 &&
      ageSeconds <= sensor.staleTimeoutSeconds && isfinite(sensor.value);
    JsonObject object = array.add<JsonObject>();
    object["name"] = sensor.name;
    object["source"] = "MQTT";
    object["id"] = sensor.id;
    object["unit"] = sensor.unit;
    object["valid"] = fresh;
    object["state"] = fresh ? "OK" : sensor.lastUpdate == 0 ? "NO DATA" : "STALE";
    if (ageSeconds == UINT32_MAX) object["ageSeconds"] = nullptr;
    else object["ageSeconds"] = ageSeconds;
    if (fresh) object["value"] = sensor.value;
    else object["value"] = nullptr;
  }
}

void ExternalSensorRegistry::readHistory(float *values, bool *valid, size_t maxValues) const {
  if (values == nullptr || valid == nullptr) return;
  for (size_t i = 0; i < maxValues; i++) {
    values[i] = 0.0f;
    valid[i] = false;
  }
  size_t limit = count_ < maxValues ? count_ : maxValues;
  for (size_t i = 0; i < limit; i++) {
    const ExternalSensor &sensor = sensors_[i];
    unsigned long age = sensor.lastUpdate == 0 ? ULONG_MAX : millis() - sensor.lastUpdate;
    uint32_t ageSeconds = age == ULONG_MAX ? UINT32_MAX : (uint32_t)(age / 1000UL);
    valid[i] = sensor.enabled && sensor.valid && sensor.lastUpdate != 0 &&
      ageSeconds <= sensor.staleTimeoutSeconds && isfinite(sensor.value);
    if (valid[i]) values[i] = sensor.value;
  }
}
