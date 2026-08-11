#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "src/common/webserver.h"

// Integration boundary for functionality that is specific to this fork.
// The upstream HeishaMon.ino only delegates setup, loop and web requests here.
void customFeaturesBegin();
void customFeaturesLoop(PubSubClient &mqttClient, const char *mqttBase);

bool customFeaturesHandleUri(struct webserver_t *client, const char *uri);
bool customFeaturesHandleArgs(struct webserver_t *client, struct arguments_t *args);
bool customFeaturesHandleCommandArgument(struct webserver_t *client,
  struct arguments_t *args);
bool customFeaturesHandleWrite(struct webserver_t *client);
bool customFeaturesHandleHeader(struct webserver_t *client, struct header_t *header);
bool customFeaturesHandleClose(struct webserver_t *client);
void customFeaturesAppendExternalSensorDiagnostics(JsonArray array);
void customFeaturesReadExternalSensorHistory(float *values, bool *valid,
  size_t maxValues);
bool customFeaturesReadExternalElectricalPower(uint8_t sourceId, float *value,
  uint32_t *ageSeconds);
bool customFeaturesHandleMqttMessage(const char *topic, const char *mqttBase,
  const uint8_t *payload, size_t length);
void customFeaturesMqttConnected(PubSubClient &client, const char *mqttBase);
void customFeaturesMqttDisconnected();
