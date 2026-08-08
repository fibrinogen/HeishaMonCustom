#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

#define EXTERNAL_SENSOR_CONFIG_VERSION 1
#define EXTERNAL_SENSOR_MAX 8
#define EXTERNAL_SENSOR_NAME_LENGTH 33
#define EXTERNAL_SENSOR_TOPIC_LENGTH 97
#define EXTERNAL_SENSOR_UNIT_LENGTH 13
#define EXTERNAL_SENSOR_MIN_STALE_SECONDS 5UL
#define EXTERNAL_SENSOR_MAX_STALE_SECONDS 86400UL

struct ExternalSensor {
  uint8_t id;
  bool enabled;
  char name[EXTERNAL_SENSOR_NAME_LENGTH];
  char mqttTopic[EXTERNAL_SENSOR_TOPIC_LENGTH];
  char unit[EXTERNAL_SENSOR_UNIT_LENGTH];
  uint32_t staleTimeoutSeconds;
  float value;
  bool valid;
  unsigned long lastUpdate;
};

class ExternalSensorRegistry {
 public:
  ExternalSensorRegistry();
  void begin(void (*logger)(char *message));

  bool upsert(JsonObjectConst object, char *message, size_t messageSize);
  bool remove(uint8_t id, char *message, size_t messageSize);
  bool read(uint8_t id, float *value, uint32_t *ageSeconds,
    char *detail, size_t detailSize) const;
  bool handleMqttMessage(const char *relativeTopic, const uint8_t *payload,
    size_t length);
  void subscribe(PubSubClient &client, const char *mqttBase);
  bool subscriptionsDirty() const { return subscriptionsDirty_; }
  void setMqttConnected(bool connected) { mqttConnected_ = connected; }
  void toJson(JsonDocument &document) const;
  void appendConditionSources(JsonArray array) const;
  void appendDiagnostics(JsonArray array) const;
  void readHistory(float *values, bool *valid, size_t maxValues) const;
  bool mqttConnected() const { return mqttConnected_; }
  uint8_t count() const { return count_; }

 private:
  ExternalSensor sensors_[EXTERNAL_SENSOR_MAX];
  uint8_t count_;
  bool mqttConnected_;
  bool subscriptionsDirty_;
  void (*logger_)(char *message);

  bool load();
  bool save() const;
  int8_t findIndex(uint8_t id) const;
  uint8_t nextId() const;
  bool validateTopic(const char *topic, char *message, size_t messageSize) const;
  bool validate(JsonObjectConst object, ExternalSensor &sensor,
    char *message, size_t messageSize) const;
  void log(const char *message) const;
};
