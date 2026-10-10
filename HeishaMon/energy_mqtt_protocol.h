#pragma once

#include "energy_reporting.h"
#include <ArduinoJson.h>
#include <cctype>
#include <cstdio>
#include <cstring>

inline void energyDiscoveryIdentity(const char *mac, const char *period,
                                    const char *metric, char *deviceId,
                                    size_t deviceSize, char *sensorId,
                                    size_t sensorSize) {
  char compactMac[13] = {};
  size_t index = 0;
  for (const char *cursor = mac; *cursor && index < sizeof(compactMac) - 1;
       cursor++) {
    if (*cursor != ':')
      compactMac[index++] = (char)std::tolower((unsigned char)*cursor);
  }
  snprintf(deviceId, deviceSize, "heishamon_%s", compactMac);
  snprintf(sensorId, sensorSize, "%s_energy_%s_%s", deviceId, period, metric);
  for (char *cursor = sensorId; *cursor; cursor++) {
    if (*cursor == '/' || *cursor == '-')
      *cursor = '_';
  }
}

inline void energyDiscoveryDocument(JsonDocument &document, const char *base,
                                    const char *hostname, const char *mac,
                                    const char *version, const char *willTopic,
                                    const char *period, const char *metric,
                                    const char *stateTopic,
                                    const char *validTopic, char *sensorId,
                                    size_t sensorSize) {
  char deviceId[40];
  energyDiscoveryIdentity(mac, period, metric, deviceId, sizeof(deviceId),
                          sensorId, sensorSize);
  bool cop = strcmp(metric, "cop") == 0;
  char name[96];
  snprintf(name, sizeof(name), "%s %s", period,
           cop ? "COP"
               : (strcmp(metric, "heat_kwh") == 0 ? "heat energy"
                                                  : "electrical energy"));
  document["unique_id"] = sensorId;
  document["name"] = name;
  document["state_topic"] = stateTopic;
  document["state_class"] = cop ? "measurement" : "total_increasing";
  document["suggested_display_precision"] = 3;
  if (cop)
    document["icon"] = "mdi:division";
  else {
    document["device_class"] = "energy";
    document["unit_of_measurement"] = "kWh";
  }
  document["availability_mode"] = "all";
  JsonArray availability = document["availability"].to<JsonArray>();
  char availabilityTopic[144];
  snprintf(availabilityTopic, sizeof(availabilityTopic), "%s/%s", base,
           willTopic);
  JsonObject connection = availability.add<JsonObject>();
  connection["topic"] = availabilityTopic;
  connection["payload_available"] = "Online";
  connection["payload_not_available"] = "Offline";
  JsonObject valid = availability.add<JsonObject>();
  valid["topic"] = validTopic;
  valid["payload_available"] = "Online";
  valid["payload_not_available"] = "Offline";
  JsonObject device = document["device"].to<JsonObject>();
  device["identifiers"].to<JsonArray>().add(deviceId);
  JsonArray network = device["connections"].to<JsonArray>().add<JsonArray>();
  network.add("mac");
  network.add(mac);
  device["name"] = hostname;
  device["manufacturer"] = "HeishaMon";
  device["model"] = "HeishaMon";
  device["sw_version"] = version;
}

template <typename Sink>
bool publishRecordedEnergyMetric(Sink &sink, const char *base,
                                 const char *period, const char *metric,
                                 double value, bool valid, bool discovery) {
  char topic[224], availability[240];
  snprintf(topic, sizeof(topic), "%s/energy/%s/%s", base, period, metric);
  snprintf(availability, sizeof(availability), "%s/availability", topic);
  bool published = true;
  if (discovery)
    published = sink.discover(period, metric, topic, availability);
  if (!valid) {
    // A retained deletion is broker control, not a numeric sensor state.
    published = sink.publish(availability, "Offline") && published;
    return sink.publish(topic, "") && published;
  }
  char payload[48];
  snprintf(payload, sizeof(payload), "%.6f", value);
  bool statePublished = sink.publish(topic, payload);
  published = statePublished && published;
  if (statePublished)
    published = sink.publish(availability, "Online") && published;
  sink.service();
  return published;
}

template <typename Sink>
bool publishRecordedEnergy(Sink &sink, const char *base, const char *period,
                           const RecordedEnergy &energy, bool discovery) {
  double cop = 0.0;
  bool validCop = energy.cop(cop);
  bool published = publishRecordedEnergyMetric(
      sink, base, period, "electrical_kwh", energy.electricalKWh,
      energy.valid(), discovery);
  published =
      publishRecordedEnergyMetric(sink, base, period, "heat_kwh",
                                  energy.heatKWh, energy.valid(), discovery) &&
      published;
  return publishRecordedEnergyMetric(sink, base, period, "cop", cop, validCop,
                                     discovery) &&
         published;
}
