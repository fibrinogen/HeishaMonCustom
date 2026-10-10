#include "energy_mqtt.h"
#include "energy_mqtt_protocol.h"

#include "commands.h"
#include "version.h"
#include "webfunctions.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#include <esp_mac.h>
#endif

extern PubSubClient mqtt_client;
extern settingsStruct heishamonSettings;

namespace {
RecordedEnergyLedger fallback;
bool fallbackLoaded = false;
bool fallbackDirty = false;
unsigned long lastFallbackSaveAt = 0;
constexpr unsigned long SAVE_INTERVAL_MS = 300000UL;

struct EnergyMqttSink {
  bool discover(const char *period, const char *metric, const char *stateTopic,
                const char *validTopic) {
    JsonDocument document;
    char sensorId[128];
#ifdef ESP32
    uint8_t macBytes[6] = {};
    esp_read_mac(macBytes, ESP_MAC_WIFI_STA);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", macBytes[0],
             macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
#else
    String address = WiFi.macAddress();
    const char *mac = address.c_str();
#endif
    energyDiscoveryDocument(document, heishamonSettings.mqtt_topic_base,
                            heishamonSettings.wifi_hostname, mac,
                            HEISHAMON_VERSION, mqtt_willtopic, period, metric,
                            stateTopic, validTopic, sensorId, sizeof(sensorId));
    String configTopic = String("homeassistant/sensor/") + sensorId + "/config";
    String payload;
    serializeJson(document, payload);
    // Stream discovery to avoid the 1024-byte PubSubClient packet limit when
    // configured prefixes, hostnames, or historical sensor names are long.
    if (!mqtt_client.beginPublish(configTopic.c_str(), payload.length(), true))
      return false;
    bool written = mqtt_client.write((const uint8_t *)payload.c_str(),
                                     payload.length()) == payload.length();
    bool ended = mqtt_client.endPublish();
    if (!written || !ended)
      mqtt_client.disconnect();
    return ended && written;
  }

  bool publish(const char *topic, const char *payload) {
    return mqtt_client.publish(topic, payload, true);
  }

  void service() {
    mqtt_client.loop();
    delay(0);
  }
};

void loadFallback() {
  if (fallbackLoaded)
    return;
  fallbackLoaded = true;
  File file = LittleFS.open("/energy.json", "r");
  if (!file)
    return;
  JsonDocument document;
  if (deserializeJson(document, file) || document["version"].as<int>() != 1)
    return;
  fallback.dayStart = document["dayStart"] | 0U;
  fallback.today.electricalKWh = document["electrical"] | 0.0;
  fallback.today.heatKWh = document["heat"] | 0.0;
  if (!fallback.today.valid())
    fallback.today = RecordedEnergy();
  for (JsonObject item : document["months"].as<JsonArray>()) {
    RecordedEnergyMonth period;
    period.year = item["year"] | 0;
    period.month = item["month"] | 0;
    period.energy.electricalKWh = item["electrical"] | -1.0;
    period.energy.heatKWh = item["heat"] | -1.0;
    if (period.year >= 2020 && period.month >= 1 && period.month <= 12 &&
        period.energy.valid())
      fallback.months.push_back(period);
  }
}

void saveFallback() {
  if (!fallbackDirty)
    return;
  JsonDocument document;
  document["version"] = 1;
  document["dayStart"] = fallback.dayStart;
  document["electrical"] = fallback.today.electricalKWh;
  document["heat"] = fallback.today.heatKWh;
  JsonArray months = document["months"].to<JsonArray>();
  for (const auto &period : fallback.months) {
    JsonObject item = months.add<JsonObject>();
    item["year"] = period.year;
    item["month"] = period.month;
    item["electrical"] = period.energy.electricalKWh;
    item["heat"] = period.energy.heatKWh;
  }
  File file = LittleFS.open("/energy.tmp", "w");
  if (!file)
    return;
  size_t expected = measureJson(document);
  bool written = serializeJson(document, file) == expected;
  file.flush();
  file.close();
  if (written && LittleFS.rename("/energy.tmp", "/energy.json")) {
    fallbackDirty = false;
    lastFallbackSaveAt = millis();
  }
}
} // namespace

void energyMqttBegin() {
  fallback = RecordedEnergyLedger();
  fallbackLoaded = fallbackDirty = false;
  lastFallbackSaveAt = 0;
}

void energyMqttInvalidateCurrent() {
  // A retained value can belong to yesterday after an offline restart. Keep
  // current-period sensors unavailable until the clock and history are ready.
  const char *periods[] = {"today", "month", "year"};
  const char *metrics[] = {"electrical_kwh", "heat_kwh", "cop"};
  for (const char *period : periods) {
    for (const char *metric : metrics) {
      char topic[224];
      snprintf(topic, sizeof(topic), "%s/energy/%s/%s/availability",
               heishamonSettings.mqtt_topic_base, period, metric);
      mqtt_client.publish(topic, "Offline", true);
    }
  }
}

bool energyMqttPublishPeriod(const char *period, const RecordedEnergy &energy,
                             bool discovery) {
  EnergyMqttSink sink;
  return publishRecordedEnergy(sink, heishamonSettings.mqtt_topic_base, period,
                               energy, discovery);
}

void energyMqttUpdateFallback(uint32_t dayStart, int year, int month,
                              const RecordedEnergy &live, bool recorded) {
  loadFallback();
  if (recorded) {
    bool rollover = fallback.dayStart != 0 && fallback.dayStart != dayStart;
    fallback.update(dayStart, year, month, live);
    fallbackDirty = true;
    if (rollover)
      saveFallback();
  }
  if (lastFallbackSaveAt == 0 ||
      (unsigned long)(millis() - lastFallbackSaveAt) >= SAVE_INTERVAL_MS)
    saveFallback();
}

bool energyMqttPublishFallback(uint32_t dayStart, int year, int month,
                               bool discovery) {
  loadFallback();
  RecordedEnergy today =
      fallback.dayStart == dayStart ? fallback.today : RecordedEnergy();
  bool published = energyMqttPublishPeriod("today", today, discovery);
  published = energyMqttPublishPeriod("month", fallback.forMonth(year, month),
                                      discovery) &&
              published;
  published =
      energyMqttPublishPeriod("year", fallback.forYear(year), discovery) &&
      published;
  for (size_t index = 0; index < fallback.months.size(); index++) {
    const auto &period = fallback.months[index];
    if (!discovery && (period.year != year || period.month != month))
      continue;
    char suffix[40];
    snprintf(suffix, sizeof(suffix), "history/month/%04d-%02d", period.year,
             period.month);
    published =
        energyMqttPublishPeriod(suffix, period.energy, discovery) && published;
  }
  for (size_t index = 0; index < fallback.months.size(); index++) {
    int recordedYear = fallback.months[index].year;
    bool seen = false;
    for (size_t before = 0; before < index; before++) {
      if (fallback.months[before].year == recordedYear)
        seen = true;
    }
    if (seen || (!discovery && recordedYear != year))
      continue;
    char suffix[40];
    snprintf(suffix, sizeof(suffix), "history/year/%04d", recordedYear);
    published = energyMqttPublishPeriod(suffix, fallback.forYear(recordedYear),
                                        discovery) &&
                published;
  }
  return published;
}
