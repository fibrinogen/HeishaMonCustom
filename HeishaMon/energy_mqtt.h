#pragma once

#include "energy_reporting.h"

void energyMqttBegin();
void energyMqttInvalidateCurrent();
bool energyMqttPublishPeriod(const char *period, const RecordedEnergy &energy,
                             bool discovery);
void energyMqttUpdateFallback(uint32_t dayStart, int year, int month,
                              const RecordedEnergy &live, bool recorded);
bool energyMqttPublishFallback(uint32_t dayStart, int year, int month,
                               bool discovery);
