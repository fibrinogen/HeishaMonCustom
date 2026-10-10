#include "diagnostics_energy.h"
#include "energy_mqtt_protocol.h"
#include <cassert>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

static RecordedEnergy energy(double electrical, double heat) {
  RecordedEnergy result;
  result.electricalKWh = electrical;
  result.heatKWh = heat;
  return result;
}

static void near(double actual, double expected) {
  assert(std::fabs(actual - expected) < 0.000001);
}

static uint32_t localTimestamp(int year, int month, int day, int hour = 0,
                               int minute = 0) {
  struct tm date = {};
  date.tm_year = year - 1900;
  date.tm_mon = month - 1;
  date.tm_mday = day;
  date.tm_hour = hour;
  date.tm_min = minute;
  date.tm_isdst = -1;
  return (uint32_t)mktime(&date);
}

struct Sink {
  std::map<std::string, std::string> retained;
  std::map<std::string, std::string> configs;
  std::string failTopic;
  int services = 0;

  bool publish(const char *topic, const char *payload) {
    if (topic == failTopic)
      return false;
    if (*payload)
      retained[topic] = payload;
    else
      retained.erase(topic);
    return true;
  }

  bool discover(const char *period, const char *metric, const char *state,
                const char *valid) {
    JsonDocument document;
    char sensorId[128];
    energyDiscoveryDocument(document, "test/pump", "Pump \"home\"",
                            "AA:BB:CC:DD:EE:FF", "test", "LWT", period, metric,
                            state, valid, sensorId, sizeof(sensorId));
    std::string json;
    serializeJson(document, json);
    configs[sensorId] = json;
    return true;
  }

  void service() { services++; }
};

static HistorySample sample(uint32_t when, int thermal100, int electricalW) {
  HistorySample result = {};
  result.timestamp = when;
  result.flags = SAMPLE_FLAG_TIME_VALID | SAMPLE_FLAG_COMPRESSOR;
  result.operatingMode = 1;
  result.thermalPower100 = thermal100;
  result.electricalPowerW = electricalW;
  result.validFields =
      HISTORY_FIELD_THERMAL_POWER | HISTORY_FIELD_ELECTRICAL_POWER;
  return result;
}

int main() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  // COP of totals differs from the average of instantaneous COPs (6 and 2).
  RecordedEnergy total = energy(1, 6);
  total.add(energy(3, 6));
  double cop = 0;
  assert(total.cop(cop));
  near(cop, 3.0);
  assert(!energy(0, 5).cop(cop));
  assert(!energy(0, 0).cop(cop));
  assert(energy(2, 0).cop(cop));
  near(cop, 0.0);
  assert(!energy(NAN, 5).cop(cop));
  assert(!energy(2, INFINITY).cop(cop));

  // Session snapshots, restart, skipped months, month and year rollovers.
  RecordedEnergyLedger ledger;
  uint32_t december = localTimestamp(2025, 12, 31);
  ledger.update(december, 2025, 12, energy(2, 8));
  ledger.update(december, 2025, 12, energy(2, 8));
  near(ledger.today.electricalKWh, 2);
  ledger.update(december, 2025, 12, energy(3, 10));
  near(ledger.forYear(2025).heatKWh, 10);
  RecordedEnergyLedger reboot;
  reboot.months = ledger.months;
  reboot.dayStart = ledger.dayStart;
  reboot.today = ledger.today;
  reboot.update(december, 2025, 12, energy(1, 4));
  near(reboot.today.electricalKWh, 4);
  near(reboot.forYear(2025).heatKWh, 14);
  reboot.update(localTimestamp(2026, 1, 1), 2026, 1, energy(1, 5));
  near(reboot.today.heatKWh, 5);
  near(reboot.forYear(2025).heatKWh, 14);
  reboot.update(localTimestamp(2026, 3, 1), 2026, 3, energy(2, 6));
  assert(reboot.months.size() == 3); // February was never recorded.
  near(reboot.forMonth(2026, 2).heatKWh, 0);
  near(reboot.forYear(2026).electricalKWh, 3);
  assert(reboot.forYear(2026).cop(cop));
  near(cop, 11.0 / 3.0);

  // Local dates and day lengths across both DST transitions and leap day.
  for (int hour : {1, 4, 12, 23}) {
    assert(recordedEnergyLocalDayStart(localTimestamp(2026, 3, 29, hour)) ==
           localTimestamp(2026, 3, 29));
    assert(recordedEnergyLocalDayStart(localTimestamp(2026, 10, 25, hour)) ==
           localTimestamp(2026, 10, 25));
  }
  assert(localTimestamp(2026, 3, 30) - localTimestamp(2026, 3, 29) ==
         23 * 3600);
  assert(localTimestamp(2026, 10, 26) - localTimestamp(2026, 10, 25) ==
         25 * 3600);
  assert(recordedEnergyLocalDayStart(localTimestamp(2024, 2, 29, 23)) ==
         localTimestamp(2024, 2, 29));

  // Split a changing power interval at New Year without losing or duplicating
  // energy. This is the same integration used by live and recovered SD days.
  uint32_t midnight = localTimestamp(2026, 1, 1);
  HistorySample first = sample(midnight - 60, 200, 1000);
  HistorySample second = sample(midnight + 60, 600, 3000);
  EnergyTotals before, after, whole;
  assert(addEnergyPair(first, second, 0, 0, midnight, before));
  assert(addEnergyPair(first, second, 0, midnight, UINT32_MAX, after));
  assert(addEnergyPair(first, second, 0, 0, UINT32_MAX, whole));
  near(before.electricalKWh, 1.5 / 60);
  near(after.electricalKWh, 2.5 / 60);
  near(before.thermalKWh + after.thermalKWh, whole.thermalKWh);
  near(before.electricalKWh + after.electricalKWh, whole.electricalKWh);
  DailySummary previousDay, currentDay;
  previousDay.valid = currentDay.valid = true;
  previousDay.dayStart = localTimestamp(2025, 12, 31);
  currentDay.dayStart = midnight;
  accumulateDailySample(previousDay, nullptr, first, 18);
  accumulateDailySample(previousDay, &first, second, 18);
  accumulateDailySample(currentDay, &first, second, 18);
  near(previousDay.heating.electricalKWh, before.electricalKWh);
  near(currentDay.heating.electricalKWh, after.electricalKWh);
  // No observations across a reboot or a long data gap means no invented
  // energy.
  EnergyTotals gap;
  second.timestamp = first.timestamp + 601;
  assert(!addEnergyPair(first, second, 0, 0, UINT32_MAX, gap));
  accumulateDailySample(currentDay, nullptr, second, 18);
  near(currentDay.heating.electricalKWh, after.electricalKWh);
  second = sample(first.timestamp + 60, 600, 3000);
  second.flags |= SAMPLE_FLAG_DEFROST;
  assert(!addEnergyPair(first, second, 0, 0, UINT32_MAX, gap));
  second = sample(first.timestamp + 60, 600, 3000);
  first.bootId = 123;
  second.bootId = 456;
  assert(!addEnergyPair(first, second, 0, 0, UINT32_MAX, gap));
  second.bootId = first.bootId;
  second.electricalSource = 1;
  assert(!addEnergyPair(first, second, 0, 0, UINT32_MAX, gap));

  Sink sink;
  assert(publishRecordedEnergy(sink, "test/pump", "today", total, true));
  assert(sink.retained.at("test/pump/energy/today/electrical_kwh") ==
         "4.000000");
  assert(sink.retained.at("test/pump/energy/today/heat_kwh") == "12.000000");
  assert(sink.retained.at("test/pump/energy/today/cop") == "3.000000");
  assert(sink.configs.size() == 3);
  JsonDocument config;
  assert(!deserializeJson(
      config,
      sink.configs.at("heishamon_aabbccddeeff_energy_today_electrical_kwh")));
  assert(config["device_class"] == "energy");
  assert(config["unit_of_measurement"] == "kWh");
  assert(config["state_class"] == "total_increasing");
  assert(config["availability_mode"] == "all");
  assert(config["availability"][0]["topic"] == "test/pump/LWT");
  assert(config["availability"][0]["payload_available"] == "Online");
  assert(config["device"]["name"] == "Pump \"home\"");
  assert(config["device"]["identifiers"][0] == "heishamon_aabbccddeeff");
  assert(config["device"]["connections"][0][1] == "AA:BB:CC:DD:EE:FF");
  assert(publishRecordedEnergy(sink, "test/pump", "history/month/2025-12",
                               total, true));
  assert(publishRecordedEnergy(sink, "test/pump", "history/year/2025", total,
                               true));
  assert(sink.configs.size() == 9);
  assert(!deserializeJson(
      config, sink.configs.at(
                  "heishamon_aabbccddeeff_energy_history_month_2025_12_cop")));
  assert(config["state_topic"] == "test/pump/energy/history/month/2025-12/cop");
  assert(config["state_class"] == "measurement");
  assert(config["unit_of_measurement"].isNull());
  // A rollover with no electricity deletes stale COP and marks it unavailable.
  assert(
      publishRecordedEnergy(sink, "test/pump", "today", energy(0, 0), false));
  assert(sink.retained.count("test/pump/energy/today/cop") == 0);
  assert(sink.retained.at("test/pump/energy/today/cop/availability") ==
         "Offline");
  // Reconnect republishes identical IDs/configs and all numeric states.
  auto ids = sink.configs;
  sink.retained.clear();
  assert(publishRecordedEnergy(sink, "test/pump", "today", total, true));
  assert(sink.configs == ids);
  assert(sink.retained.at("test/pump/energy/today/cop/availability") ==
         "Online");
  sink.failTopic = "test/pump/energy/today/electrical_kwh";
  assert(!publishRecordedEnergy(sink, "test/pump", "today", total, true));
  // Prefix/hostname changes leave device-specific identities unchanged.
  char changedId[128];
  config.clear();
  energyDiscoveryDocument(
      config, "new/prefix", "new name", "AA:BB:CC:DD:EE:FF", "new version",
      "LWT", "today", "cop", "new/prefix/energy/today/cop",
      "new/prefix/energy/today/cop/availability", changedId, sizeof(changedId));
  assert(std::string(changedId) == "heishamon_aabbccddeeff_energy_today_cop");
  // All non-control state payloads are single finite numbers.
  for (const auto &message : sink.retained) {
    if (message.first.find("/availability") != std::string::npos)
      continue;
    char *end = nullptr;
    double parsed = strtod(message.second.c_str(), &end);
    assert(end != message.second.c_str() && *end == 0 && std::isfinite(parsed));
  }
}
