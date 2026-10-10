#pragma once

#include <cmath>
#include <cstdint>
#include <ctime>
#include <vector>

struct RecordedEnergy {
  double electricalKWh = 0.0;
  double heatKWh = 0.0;

  bool valid() const {
    return std::isfinite(electricalKWh) && electricalKWh >= 0.0 &&
           std::isfinite(heatKWh) && heatKWh >= 0.0;
  }

  bool cop(double &value) const {
    if (!valid() || electricalKWh <= 0.0)
      return false;
    value = heatKWh / electricalKWh;
    return std::isfinite(value);
  }

  void add(const RecordedEnergy &other) {
    electricalKWh += other.electricalKWh;
    heatKWh += other.heatKWh;
  }
};

struct RecordedEnergyMonth {
  int year = 0;
  int month = 0;
  RecordedEnergy energy;
};

// Sparse fallback archive for boards without SD history. Only actual samples
// create entries. Session deltas let a saved day survive reboot without adding
// the same live summary repeatedly; no energy is extrapolated over downtime.
struct RecordedEnergyLedger {
  std::vector<RecordedEnergyMonth> months;
  uint32_t dayStart = 0;
  RecordedEnergy today;
  uint32_t sessionDay = 0;
  RecordedEnergy sessionEnergy;

  void update(uint32_t day, int year, int month, const RecordedEnergy &live) {
    if (day == 0 || !live.valid())
      return;
    if (sessionDay != day) {
      sessionDay = day;
      sessionEnergy = RecordedEnergy();
    }
    RecordedEnergy delta;
    delta.electricalKWh =
        std::fmax(0.0, live.electricalKWh - sessionEnergy.electricalKWh);
    delta.heatKWh = std::fmax(0.0, live.heatKWh - sessionEnergy.heatKWh);
    sessionEnergy = live;
    if (dayStart != day) {
      dayStart = day;
      today = RecordedEnergy();
    }
    today.add(delta);
    for (auto &period : months) {
      if (period.year == year && period.month == month) {
        period.energy.add(delta);
        return;
      }
    }
    RecordedEnergyMonth period;
    period.year = year;
    period.month = month;
    period.energy = delta;
    months.push_back(period);
  }

  RecordedEnergy forMonth(int year, int month) const {
    for (const auto &period : months) {
      if (period.year == year && period.month == month)
        return period.energy;
    }
    return RecordedEnergy();
  }

  RecordedEnergy forYear(int year) const {
    RecordedEnergy result;
    for (const auto &period : months) {
      if (period.year == year)
        result.add(period.energy);
    }
    return result;
  }
};

// Let libc determine DST at midnight, rather than inheriting the current
// hour's DST flag (which can differ on a clock-change day).
inline uint32_t recordedEnergyLocalDayStart(uint32_t timestamp) {
  time_t value = timestamp;
  struct tm local = {};
  if (localtime_r(&value, &local) == nullptr)
    return 0;
  local.tm_hour = local.tm_min = local.tm_sec = 0;
  local.tm_isdst = -1;
  time_t start = mktime(&local);
  return start > 0 ? (uint32_t)start : 0;
}
