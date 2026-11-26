// SPDX-License-Identifier: GPL-3.0-only
//
// Donor -> PowerReadings adapter for a single power channel. The donor is a
// Zephyr env/power sensor reading (ZephyrEnvSensors.h); its voltage_v (V) and
// current_a (A) feed one of the eight PowerReadings channels.
#pragma once
#include "PowerReadings.h"
#include "ZephyrEnvSensors.h"
#include <float.h>
namespace zt::telemetry {
struct PowerChannel { Observed<float> voltage, current; };
inline PowerChannel measuredPowerChannel(const env_sensor_reading &donor) {
  PowerChannel reading{};
  if (donor.fields & ENV_F_VOLTAGE) {
    const float v = donor.voltage_v;
    if ((v >= -FLT_MAX && v <= FLT_MAX) && v >= 0.0f) {
      reading.voltage = v;  // volts, copied unchanged
    }
  }
  if (donor.fields & ENV_F_CURRENT) {
    const float a = donor.current_a;
    if ((a >= -FLT_MAX && a <= FLT_MAX)) {
      const float ma = a * 1000.0f;  // A -> wire mA, exactly once
      if ((ma >= -FLT_MAX && ma <= FLT_MAX)) {
        reading.current = ma;  // signed; negative = reverse flow
      }
    }
  }
  return reading;
}
inline bool assignPowerChannel(PowerReadings &readings,unsigned index,const env_sensor_reading &donor) {
  if(index>=8)return false;
  const auto sampled=measuredPowerChannel(donor);
  readings.voltage[index]=sampled.voltage;
  readings.current[index]=sampled.current;
  return true;
}
}
