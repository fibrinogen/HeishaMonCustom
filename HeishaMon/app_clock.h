#pragma once

#include <BridgeTime.h>

constexpr bridge_time::Clock AppClock(
    bridge_time::ClockPolicy(0, bridge_time::EpochBoundary::Exclusive, 2024));
constexpr bridge_time::Clock EventClock(bridge_time::ClockPolicy(0));
