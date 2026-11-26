// environment_metrics_adapter_tests.cpp
// M107 fixture: toMeshtasticEnvironmentMetrics contract (C assert only).
#include <cassert>
#include <cfloat>
#include <cmath>

#include "environment/EnvironmentMetricsAdapter.h"
#include "meshtastic/mesh.pb.h"

int main() {
  // 1) All fields absent: every generated has_* flag stays false.
  {
    zt::telemetry::EnvironmentReadings r;
    assert(!toMeshtasticEnvironmentMetrics(r).has_temperature);
    assert(!toMeshtasticEnvironmentMetrics(r).has_relative_humidity);
    assert(!toMeshtasticEnvironmentMetrics(r).has_barometric_pressure);
  }
  // 2) Present zero Celsius and zero RH stay present; pressure absent.
  {
    zt::telemetry::EnvironmentReadings r;
    r.ambientCelsius = zt::Observed<float>(0.0f);
    r.humidityPercent = zt::Observed<float>(0.0f);
    auto out = toMeshtasticEnvironmentMetrics(r);
    assert(out.has_temperature && out.temperature == 0.0f);
    assert(out.has_relative_humidity && out.relative_humidity == 0.0f);
    assert(!out.has_barometric_pressure);
  }
  // 3) Finite extreme temperatures, humidity 0/100, positive pressure.
  {
    zt::telemetry::EnvironmentReadings r;
    r.ambientCelsius = zt::Observed<float>(FLT_MAX);
    r.humidityPercent = zt::Observed<float>(100.0f);
    r.pressureHpa = zt::Observed<float>(1013.25f);
    auto out = toMeshtasticEnvironmentMetrics(r);
    assert(out.has_temperature && out.temperature == FLT_MAX);
    assert(out.has_relative_humidity && out.relative_humidity == 100.0f);
    assert(out.has_barometric_pressure && out.barometric_pressure == 1013.25f);
  }
  {
    zt::telemetry::EnvironmentReadings r;
    r.ambientCelsius = zt::Observed<float>(-FLT_MAX);
    r.humidityPercent = zt::Observed<float>(0.0f);
    auto out = toMeshtasticEnvironmentMetrics(r);
    assert(out.has_temperature && out.temperature == -FLT_MAX);
    assert(out.has_relative_humidity && out.relative_humidity == 0.0f);
  }
  // 4) NaN is invalid: omitted, never passed through.
  {
    zt::telemetry::EnvironmentReadings r;
    r.ambientCelsius = zt::Observed<float>(NAN);
    assert(!toMeshtasticEnvironmentMetrics(r).has_temperature);
    assert(!toMeshtasticEnvironmentMetrics(r).has_relative_humidity);
    assert(!toMeshtasticEnvironmentMetrics(r).has_barometric_pressure);
  }
  // 5/6) +/-infinity are invalid: omitted.
  {
    zt::telemetry::EnvironmentReadings r;
    r.ambientCelsius = zt::Observed<float>(INFINITY);
    assert(!toMeshtasticEnvironmentMetrics(r).has_temperature);
    r.ambientCelsius = zt::Observed<float>(-INFINITY);
    assert(!toMeshtasticEnvironmentMetrics(r).has_temperature);
  }
  // 7) Humidity outside inclusive [0,100] is omitted.
  {
    zt::telemetry::EnvironmentReadings r;
    r.humidityPercent = zt::Observed<float>(-0.01f);
    assert(!toMeshtasticEnvironmentMetrics(r).has_relative_humidity);
    r.humidityPercent = zt::Observed<float>(100.01f);
    assert(!toMeshtasticEnvironmentMetrics(r).has_relative_humidity);
  }
  // 8) Pressure must be > 0: zero and negative are omitted.
  {
    zt::telemetry::EnvironmentReadings r;
    r.pressureHpa = zt::Observed<float>(0.0f);
    assert(!toMeshtasticEnvironmentMetrics(r).has_barometric_pressure);
    r.pressureHpa = zt::Observed<float>(-10.0f);
    assert(!toMeshtasticEnvironmentMetrics(r).has_barometric_pressure);
  }
  return 0;
}
