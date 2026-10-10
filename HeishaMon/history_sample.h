#pragma once

#include "history_config.h"
#include <cstdint>

// Compact, fixed-size sample. Temperatures are stored in 0.1 °C, flow in
// 0.01 l/min, compressor frequency in 0.1 Hz and power in 0.01 kW.
struct HistorySample {
  uint32_t sequence;
  uint32_t bootId; // Distinguish recorded sessions when replaying SD samples.
  uint32_t timestamp;
  uint32_t uptimeSeconds;
  int16_t outsideTemp10;
  int16_t inletTemp10;
  int16_t outletTemp10;
  int16_t targetTemp10;
  int16_t dhwTemp10;
  int16_t dhwTargetTemp10;
  int16_t roomTemp10;
  int16_t roomTarget10;
  int16_t evaOutletTemp10;
  uint16_t flow100;
  uint16_t compressorHz10;
  uint16_t compressorCurrent10;
  uint16_t pumpRpm;
  int16_t thermalPower100;
  uint16_t electricalPowerW;
  uint16_t heatProductionW;
  uint16_t heatConsumptionW;
  uint16_t dhwProductionW;
  uint16_t dhwConsumptionW;
  uint8_t operatingMode;
  uint8_t valveState;
  uint8_t flags;
  uint8_t operatingState;
  uint8_t electricalSource;
  int16_t zone1RequestValue10;
  uint8_t zone1RequestSemantic;
  int8_t heatingCurveShift;
  uint32_t validFields;
  float externalValues[HEISHAMON_HISTORY_EXTERNAL_SENSOR_MAX];
};

enum HistorySampleField : uint32_t {
  HISTORY_FIELD_OUTSIDE = 1u << 0,
  HISTORY_FIELD_INLET = 1u << 1,
  HISTORY_FIELD_OUTLET = 1u << 2,
  HISTORY_FIELD_TARGET = 1u << 3,
  HISTORY_FIELD_DHW = 1u << 4,
  HISTORY_FIELD_FLOW = 1u << 5,
  HISTORY_FIELD_COMPRESSOR_HZ = 1u << 6,
  HISTORY_FIELD_PUMP_RPM = 1u << 7,
  HISTORY_FIELD_THERMAL_POWER = 1u << 8,
  HISTORY_FIELD_ELECTRICAL_POWER = 1u << 9,
  HISTORY_FIELD_DHW_TARGET = 1u << 10,
  HISTORY_FIELD_ROOM = 1u << 11,
  HISTORY_FIELD_ROOM_TARGET = 1u << 12,
  HISTORY_FIELD_HEAT_PRODUCTION = 1u << 13,
  HISTORY_FIELD_HEAT_CONSUMPTION = 1u << 14,
  HISTORY_FIELD_DHW_PRODUCTION = 1u << 15,
  HISTORY_FIELD_DHW_CONSUMPTION = 1u << 16,
  HISTORY_FIELD_EXTERNAL_0 = 1u << 17,
  HISTORY_FIELD_EXTERNAL_1 = 1u << 18,
  HISTORY_FIELD_EXTERNAL_2 = 1u << 19,
  HISTORY_FIELD_EXTERNAL_3 = 1u << 20,
  HISTORY_FIELD_EXTERNAL_4 = 1u << 21,
  HISTORY_FIELD_EXTERNAL_5 = 1u << 22,
  HISTORY_FIELD_EXTERNAL_6 = 1u << 23,
  HISTORY_FIELD_EXTERNAL_7 = 1u << 24,
  HISTORY_FIELD_ZONE1_REQUEST = 1u << 25,
  HISTORY_FIELD_HEATING_CURVE_SHIFT = 1u << 26,
  HISTORY_FIELD_INTERNAL_HEATER_STATE = 1u << 27,
  HISTORY_FIELD_EXTERNAL_HEATER_STATE = 1u << 28,
  HISTORY_FIELD_EVA_OUTLET = 1u << 29,
  HISTORY_FIELD_COMPRESSOR_CURRENT = 1u << 30
};

enum HistoryOperatingState : uint8_t {
  HISTORY_STATE_UNKNOWN = 0,
  HISTORY_STATE_STANDBY,
  HISTORY_STATE_HEATING,
  HISTORY_STATE_DHW,
  HISTORY_STATE_DEFROST,
  HISTORY_STATE_CIRCULATION,
  HISTORY_STATE_TRANSITION
};
