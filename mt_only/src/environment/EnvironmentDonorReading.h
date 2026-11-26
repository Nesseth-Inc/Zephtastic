// SPDX-License-Identifier: GPL-3.0-only
// Maps a donor env_sensor_reading (ZephyrEnvSensors.h) into the canonical
// zt::telemetry::EnvironmentReadings.
#pragma once

#include "ZephyrEnvSensors.h"          // donor: env_sensor_reading, ENV_F_* bits
#include "EnvironmentMetricsAdapter.h" // canonical: zt::telemetry::EnvironmentReadings
#include "Observed.h"                  // zt::Observed<T>: default=absent, value=present

namespace zt::telemetry {

// Reports only fields whose ENV_F_* bit is set. An unset bit leaves the member
// default-constructed (absent) even if its float is nonzero; a set bit is
// present, including zero. No fallback/conversion/allocation/OS/hardware.
// Pressure is hPa as-is. altitude/luminosity/voltage/current/power unmapped.
inline EnvironmentReadings fromDonorReading(const env_sensor_reading& r) {
  EnvironmentReadings out;
  if (r.fields & ENV_F_TEMPERATURE) {
    out.ambientCelsius = Observed<float>(r.temperature_c);
  }
  if (r.fields & ENV_F_HUMIDITY) {
    out.humidityPercent = Observed<float>(r.humidity_pct);
  }
  if (r.fields & ENV_F_PRESSURE) {
    out.pressureHpa = Observed<float>(r.pressure_hpa);
  }
  return out;
}

}  // namespace zt::telemetry
