// SPDX-License-Identifier: GPL-3.0-only
#include "environment/EnvironmentMetricsAdapter.h"
#include <cassert>
#include <cstdio>

using Source = zt::telemetry::EnvironmentSource;
enum : unsigned { Temperature = 1, Humidity = 2, Pressure = 4, ClearHumidity = 8 };
struct Sensor { Source source; unsigned fields; };
constexpr Sensor registered[] = {
#include "registered.inc"
};

int main() {
  constexpr unsigned count = sizeof(registered)/sizeof(registered[0]);
  for (unsigned mask=0; mask<(1u<<count); ++mask) {
    meshtastic_EnvironmentMetrics expected{};
    zt::telemetry::EnvironmentReadings readings;
    auto &baro=readings.accessory.barometer;
    // The oracle applies the selected writes in the pinned constructor order.
    for (unsigned i=0; i<count; ++i) {
      if (!(mask & (1u<<i))) continue;
      auto [source,fields]=registered[i];
      const float value=i+1;
      if ((fields&Temperature) && (source!=Source::Aht || !expected.has_temperature)) {
        expected.has_temperature=true;expected.temperature=value;
      }
      if ((fields&Humidity) && (source!=Source::Aht || !expected.has_relative_humidity)) {
        expected.has_relative_humidity=true;expected.relative_humidity=value;
      }
      if (fields&Pressure) {expected.has_barometric_pressure=true;expected.barometric_pressure=900+value;}
      if (fields&ClearHumidity) {expected.has_relative_humidity=false;expected.relative_humidity=0;}
      if (source==Source::Aht || source==Source::Bmp085 || source==Source::Dps310 || source==Source::Bmp3xx) {
        mt::stock::telemetry::accessory::BarometerSample sample;
        sample.hasTemperature=fields&Temperature;sample.temperature=value;sample.temperatureOrigin=source;
        sample.hasHumidity=fields&Humidity;sample.clearHumidity=fields&ClearHumidity;
        sample.humidity=value;sample.humidityOrigin=source;
        sample.hasPressure=fields&Pressure;sample.pressure=900+value;sample.pressureOrigin=source;
        baro.merge(sample);
      }
    }
    // Native probes, cached infrared and OneWire can be delivered in any order.
    for (unsigned i=count; i--;) {
      if (!(mask & (1u<<i))) continue;
      auto [source,fields]=registered[i];
      const float value=i+1;
      if (source==Source::Lark) {
        auto &lark=readings.accessory;
        lark.hasWeather=lark.hasWeatherHumidity=true;
        lark.temperature=value;lark.humidity=value;lark.pressure=900+value;
      } else if (source!=Source::Aht && source!=Source::Bmp085 && source!=Source::Dps310 && source!=Source::Bmp3xx) {
        if(fields&Temperature)readings.ambientCelsius.record(source,value);
        if(fields&Humidity)readings.humidityPercent.record(source,value);
        if(fields&Pressure)readings.pressureHpa.record(source,900+value);
      }
    }
    const auto actual=zt::telemetry::toMeshtasticEnvironmentMetrics(readings);
    assert(actual.has_temperature==expected.has_temperature && actual.temperature==expected.temperature);
    assert(actual.has_relative_humidity==expected.has_relative_humidity && actual.relative_humidity==expected.relative_humidity);
    assert(actual.has_barometric_pressure==expected.has_barometric_pressure && actual.barometric_pressure==expected.barometric_pressure);
  }
  std::printf("%u fitted-source combinations follow the stock T/RH/pressure field order\n",1u<<count);
}
