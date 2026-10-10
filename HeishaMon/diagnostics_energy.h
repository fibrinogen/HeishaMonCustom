#pragma once

#include "diagnostics_logic.h"
#include "energy_reporting.h"
#include "history_sample.h"

constexpr uint8_t SAMPLE_FLAG_COMPRESSOR = 0x01;
constexpr uint8_t SAMPLE_FLAG_HEATPUMP = 0x02;
constexpr uint8_t SAMPLE_FLAG_DHW = 0x04;
constexpr uint8_t SAMPLE_FLAG_DEFROST = 0x08;
constexpr uint8_t SAMPLE_FLAG_TIME_VALID = 0x10;
constexpr uint8_t SAMPLE_FLAG_EXTERNAL_ELECTRICAL = 0x20;
constexpr uint8_t SAMPLE_FLAG_INTERNAL_HEATER = 0x40;
constexpr uint8_t SAMPLE_FLAG_EXTERNAL_HEATER = 0x80;

// 1 = heating, 2 = DHW. Cooling and unknown modes are deliberately excluded:
// the water-side formula is not a trustworthy cooling COP measurement.
inline int efficiencyGroup(const HistorySample &sample) {
  if ((sample.flags & SAMPLE_FLAG_COMPRESSOR) == 0 ||
      (sample.flags & SAMPLE_FLAG_DEFROST) != 0)
    return 0;
  switch (sample.operatingMode) {
  case 1: // Heat
  case 3: // Auto(heat)
    return (sample.flags & SAMPLE_FLAG_DHW) != 0 ? 2 : 1;
  case 4: // DHW
  case 5: // Heat+DHW
  case 7: // Auto(heat)+DHW
    return (sample.flags & SAMPLE_FLAG_DHW) != 0 ? 2 : 1;
  default:
    return 0;
  }
}

struct EnergyTotals {
  double thermalKWh = 0.0;
  double electricalKWh = 0.0;
  uint32_t intervals = 0;
};

struct DailySummary {
  bool valid = false;
  uint32_t dayStart = 0;
  double heatingDegreeDays = 0.0;
  EnergyTotals heating;
  EnergyTotals dhw;
  uint32_t compressorSeconds = 0;
  uint32_t compressorStarts = 0;
  float outsideMinimum = NAN;
  float outsideMaximum = NAN;
  double outsideSum = 0.0;
  uint32_t outsideSamples = 0;
};

inline bool addEnergyPair(const HistorySample &previous,
                          const HistorySample &current, int groupFilter,
                          uint32_t lowerTime, uint32_t upperTime,
                          EnergyTotals &totals) {
  if ((previous.flags & SAMPLE_FLAG_TIME_VALID) == 0 ||
      (current.flags & SAMPLE_FLAG_TIME_VALID) == 0)
    return false;
  if (previous.bootId != current.bootId ||
      previous.electricalSource != current.electricalSource)
    return false;
  uint32_t previousTime = previous.timestamp;
  uint32_t currentTime = current.timestamp;
  if (currentTime <= previousTime || currentTime - previousTime > 600UL ||
      currentTime <= lowerTime || previousTime >= upperTime)
    return false;
  uint32_t start = previousTime < lowerTime ? lowerTime : previousTime;
  uint32_t stop = currentTime > upperTime ? upperTime : currentTime;
  double firstFraction =
      (double)(start - previousTime) / (currentTime - previousTime);
  double lastFraction =
      (double)(stop - previousTime) / (currentTime - previousTime);
  int previousGroup = efficiencyGroup(previous);
  int currentGroup = efficiencyGroup(current);
  if (previousGroup == 0 || currentGroup == 0 ||
      previousGroup != currentGroup ||
      (groupFilter != 0 && currentGroup != groupFilter))
    return false;
  float previousThermal = previous.thermalPower100 / 100.0f;
  float currentThermal = current.thermalPower100 / 100.0f;
  float previousElectrical = previous.electricalPowerW / 1000.0f;
  float currentElectrical = current.electricalPowerW / 1000.0f;
  bool thermalAdded =
      (previous.validFields & HISTORY_FIELD_THERMAL_POWER) != 0 &&
      (current.validFields & HISTORY_FIELD_THERMAL_POWER) != 0 &&
      previousThermal > 0.1f && currentThermal > 0.1f &&
      diagnosticsIntegratePower(
          previousThermal + (currentThermal - previousThermal) * firstFraction,
          previousThermal + (currentThermal - previousThermal) * lastFraction,
          stop - start, totals.thermalKWh);
  bool electricalAdded =
      (previous.validFields & HISTORY_FIELD_ELECTRICAL_POWER) != 0 &&
      (current.validFields & HISTORY_FIELD_ELECTRICAL_POWER) != 0 &&
      previous.electricalPowerW >= 100.0f &&
      current.electricalPowerW >= 100.0f &&
      diagnosticsIntegratePower(
          previousElectrical +
              (currentElectrical - previousElectrical) * firstFraction,
          previousElectrical +
              (currentElectrical - previousElectrical) * lastFraction,
          stop - start, totals.electricalKWh);
  if (thermalAdded && electricalAdded)
    totals.intervals++;
  return thermalAdded || electricalAdded;
}

inline void accumulateDailySample(DailySummary &summary,
                                  const HistorySample *previous,
                                  const HistorySample &sample,
                                  double degreeDayBase) {
  uint32_t sampleDay = recordedEnergyLocalDayStart(sample.timestamp);
  if (sampleDay == summary.dayStart) {
    if ((sample.validFields & HISTORY_FIELD_OUTSIDE) != 0) {
      float outside = sample.outsideTemp10 / 10.0f;
      summary.outsideSum += outside;
      summary.outsideSamples++;
      if (!std::isfinite(summary.outsideMinimum) ||
          outside < summary.outsideMinimum)
        summary.outsideMinimum = outside;
      if (!std::isfinite(summary.outsideMaximum) ||
          outside > summary.outsideMaximum)
        summary.outsideMaximum = outside;
    }
  }
  if (previous == nullptr || (previous->flags & SAMPLE_FLAG_TIME_VALID) == 0)
    return;
  uint32_t previousTime = previous->timestamp;
  uint32_t currentTime = sample.timestamp;
  if (currentTime <= previousTime || currentTime - previousTime > 600UL)
    return;
  uint32_t upper = sampleDay > summary.dayStart ? sampleDay : currentTime;
  uint32_t lower =
      previousTime < summary.dayStart ? summary.dayStart : previousTime;
  if (upper <= lower)
    return;
  if ((previous->validFields & HISTORY_FIELD_OUTSIDE) != 0 &&
      (sample.validFields & HISTORY_FIELD_OUTSIDE) != 0) {
    summary.heatingDegreeDays += diagnosticsHeatingDegreeDays(
        degreeDayBase, previous->outsideTemp10 / 10.0,
        sample.outsideTemp10 / 10.0, upper - lower);
  }
  addEnergyPair(*previous, sample, 1, summary.dayStart, upper, summary.heating);
  addEnergyPair(*previous, sample, 2, summary.dayStart, upper, summary.dhw);
  if ((sample.flags & SAMPLE_FLAG_COMPRESSOR) != 0)
    summary.compressorSeconds += upper - lower;
  if (sampleDay == summary.dayStart &&
      (sample.flags & SAMPLE_FLAG_COMPRESSOR) != 0 &&
      (previous->flags & SAMPLE_FLAG_COMPRESSOR) == 0)
    summary.compressorStarts++;
}
