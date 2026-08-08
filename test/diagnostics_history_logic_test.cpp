#include <assert.h>
#include <math.h>

#include "diagnostics_logic.h"

int main() {
  assert(fabsf((14.0f * 5.0f * 0.0697f) - 4.879f) < 0.001f);
  float power = 0;
  assert(diagnosticsCalculateThermalPower(14.0f, 5.0f, true, true, true, power));
  assert(fabsf(power - 4.879f) < 0.001f);
  assert(!diagnosticsCalculateThermalPower(0.0f, 5.0f, true, true, true, power));
  assert(!diagnosticsCalculateThermalPower(14.0f, 5.0f, true, false, true, power));
  assert(!diagnosticsCalculateThermalPower(14.0f, 5.0f, true, true, false, power));
  assert(diagnosticsValidTemperature(20.0f));
  assert(!diagnosticsValidTemperature(-128.0f));
  assert(!diagnosticsValidTemperature(NAN));

  DiagnosticsCycleTracker tracker;
  assert(diagnosticsUpdateCycle(tracker, false, 10) == DIAGNOSTICS_CYCLE_NONE);
  assert(diagnosticsUpdateCycle(tracker, false, 20) == DIAGNOSTICS_CYCLE_NONE);
  assert(diagnosticsUpdateCycle(tracker, true, 30) == DIAGNOSTICS_CYCLE_STARTED);
  assert(tracker.starts == 1);
  assert(diagnosticsUpdateCycle(tracker, true, 40) == DIAGNOSTICS_CYCLE_NONE);
  assert(diagnosticsUpdateCycle(tracker, false, 75) == DIAGNOSTICS_CYCLE_STOPPED);
  assert(tracker.previousRunSeconds == 45);
  assert(tracker.totalRunSeconds == 45);
  assert(tracker.starts == 1);
  assert(diagnosticsUpdateCycle(tracker, true, 100) == DIAGNOSTICS_CYCLE_STARTED);
  assert(diagnosticsUpdateCycle(tracker, false, 125) == DIAGNOSTICS_CYCLE_STOPPED);
  assert(tracker.starts == 2);
  assert(tracker.totalRunSeconds == 70);
  return 0;
}
