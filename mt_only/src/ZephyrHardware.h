// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "HardwareSnapshot.h"
namespace mt::hardware {
// Main thread only, outside ServiceLock. No flash, RF, identity or policy IO.
void init();
bool siliconId(uint8_t (&out)[16]);
// Main-thread battery divider calibration. Zero restores the devicetree scale;
// unsupported hardware still reports no battery reading.
void setAdcMultiplierOverride(float multiplier);
// Main-thread explicit INA selector. Zero restores the devicetree ADC source;
// a selected but failing/absent INA never falls back to ADC.
bool setInaBatteryAddress(uint32_t address);
void poll(uint64_t now);
HardwareSnapshot snapshot(uint64_t now);
}
