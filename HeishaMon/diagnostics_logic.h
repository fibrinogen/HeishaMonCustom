#pragma once

#include <cmath>
#include <cstdint>

inline bool diagnosticsValidTemperature(float value) {
  return std::isfinite(value) && value >= -60.0f && value <= 150.0f;
}

inline bool diagnosticsCalculateThermalPower(float flowLitersPerMinute,
    float deltaKelvin, bool flowValid, bool deltaValid, bool waterLiquid,
    float &powerKw) {
  if (!flowValid || !deltaValid || !waterLiquid ||
      !std::isfinite(flowLitersPerMinute) || flowLitersPerMinute <= 0.2f ||
      !std::isfinite(deltaKelvin)) return false;
  powerKw = flowLitersPerMinute * deltaKelvin * 0.0697f;
  return std::isfinite(powerKw);
}

struct DiagnosticsCycleTracker {
  bool initialized = false;
  bool running = false;
  uint32_t startedAt = 0;
  uint32_t previousRunSeconds = 0;
  uint32_t starts = 0;
  uint64_t totalRunSeconds = 0;
};

enum DiagnosticsCycleTransition : uint8_t {
  DIAGNOSTICS_CYCLE_NONE = 0,
  DIAGNOSTICS_CYCLE_STARTED,
  DIAGNOSTICS_CYCLE_STOPPED
};

inline DiagnosticsCycleTransition diagnosticsUpdateCycle(
    DiagnosticsCycleTracker &tracker, bool running, uint32_t nowSeconds) {
  if (!tracker.initialized) {
    tracker.initialized = true;
    tracker.running = running;
    tracker.startedAt = running ? nowSeconds : 0;
    return DIAGNOSTICS_CYCLE_NONE;
  }
  if (tracker.running == running) return DIAGNOSTICS_CYCLE_NONE;
  tracker.running = running;
  if (running) {
    tracker.startedAt = nowSeconds;
    tracker.starts++;
    return DIAGNOSTICS_CYCLE_STARTED;
  }
  tracker.previousRunSeconds = tracker.startedAt == 0 || nowSeconds < tracker.startedAt ?
    0 : nowSeconds - tracker.startedAt;
  tracker.totalRunSeconds += tracker.previousRunSeconds;
  tracker.startedAt = 0;
  return DIAGNOSTICS_CYCLE_STOPPED;
}
