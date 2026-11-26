// SPDX-License-Identifier: GPL-3.0-only
// Pure C++17 adapter: maps validated zt::telemetry environment readings onto
// a zero-initialized meshtastic_EnvironmentMetrics. A field is set only when
// the corresponding zt::Observed input is present and finite; absent, NaN,
// infinite, or out-of-range inputs stay absent. Nothing is invented.
#pragma once

#include <float.h>

#include "Observed.h"
#include "ClimateField.h"
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
#include "stock/telemetry/accessory/RegisterDevices.h"
#endif
#include "meshtastic/telemetry.pb.h"

#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || defined(CONFIG_MT_STOCK_INA_METRICS) || defined(CONFIG_MT_ACCESSORY_SENSORS)
#include "stock/telemetry/EnvironmentChannels.h"
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
#include "stock/telemetry/OneWireThermometers.h"
#endif
namespace zt::telemetry {

// Ambient environment inputs. Each field is a zt::Observed<float>: a present
// zero (0 C, 0 % RH) is meaningful and distinct from absence.
struct EnvironmentReadings {
#if defined(CONFIG_MT_BME680_IAQ)
  zt::Observed<float> bmeGas;
  zt::Observed<uint32_t> bmeIaq;
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  mt::stock::telemetry::accessory::Environment accessory{};
#endif
  ClimateField ambientCelsius;  // Ambient temperature, Celsius.
  ClimateField humidityPercent; // Relative humidity, percent (0..100).
  ClimateField pressureHpa;     // Barometric pressure, hPa (>0).
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || defined(CONFIG_MT_STOCK_INA_METRICS)
  mt::stock::telemetry::EnvironmentChannels channels{};
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
  mt::stock::telemetry::OneWireMeasurements oneWire{};
#endif
};

// Copies only present-and-valid inputs into a zero-initialized Meshtastic
// metrics struct. Temperature accepts any finite Celsius value. Humidity must
// be finite and within 0..100 inclusive. Pressure must be finite and >0.
// Missing, NaN, infinite, out-of-range humidity, or zero/negative pressure
// remain absent. No coordinates, discovery, selectors, settings, logging,
// or allocation.
#if defined(CONFIG_MT_LARK_WEATHER)
inline void applyWind(meshtastic_EnvironmentMetrics &m,bool hasSpeed,float speed,bool hasDirection,unsigned direction) {
  if(hasSpeed&&speed>=0&&speed<=FLT_MAX){m.has_wind_speed=true;m.wind_speed=speed;}
  if(hasDirection&&direction<360){m.has_wind_direction=true;m.wind_direction=direction;}
}
#endif
inline meshtastic_EnvironmentMetrics toMeshtasticEnvironmentMetrics(
    const EnvironmentReadings &readings) {
  meshtastic_EnvironmentMetrics m{};
  auto temperature = readings.ambientCelsius;
  auto humidity = readings.humidityPercent;
  auto pressure = readings.pressureHpa;
#if defined(CONFIG_MT_LARK_WEATHER)
  const auto &weather = readings.accessory;
  if (weather.hasWeather) {
    if (weather.temperature >= -FLT_MAX && weather.temperature <= FLT_MAX)
      temperature.record(EnvironmentSource::Lark, weather.temperature);
    if (weather.hasWeatherHumidity && weather.humidity >= 0 && weather.humidity <= 100)
      humidity.record(EnvironmentSource::Lark, weather.humidity);
    if (weather.pressure > 0 && weather.pressure <= FLT_MAX)
      pressure.record(EnvironmentSource::Lark, weather.pressure);
  }
#endif
#if defined(CONFIG_MT_STOCK_BAROMETERS)
  const auto &barometer = readings.accessory.barometer;
  if (barometer.hasTemperature) temperature.record(barometer.temperatureOrigin, barometer.temperature);
  if (barometer.hasPressure) pressure.record(barometer.pressureOrigin, barometer.pressure);
  if (barometer.clearHumidity) humidity.clear(barometer.humidityOrigin);
  if (barometer.hasHumidity) humidity.record(barometer.humidityOrigin, barometer.humidity);
#endif
#if defined(CONFIG_MT_STOCK_SHT)
  if (readings.accessory.sht.present) {
    temperature.record(EnvironmentSource::Sht, readings.accessory.sht.temperature);
    humidity.record(EnvironmentSource::Sht, readings.accessory.sht.humidity);
  }
#endif
  if (temperature && *temperature >= -FLT_MAX && *temperature <= FLT_MAX) {
    m.has_temperature = true;
    m.temperature = *temperature;
  }
  if (humidity && *humidity >= 0 && *humidity <= 100) {
    m.has_relative_humidity = true;
    m.relative_humidity = *humidity;
  }
  if (pressure && *pressure > 0 && *pressure <= FLT_MAX) {
    m.has_barometric_pressure = true;
    m.barometric_pressure = *pressure;
  }
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || defined(CONFIG_MT_STOCK_INA_METRICS)
  readings.channels.apply(m);
#endif
#if defined(CONFIG_MT_BME680_IAQ)
  if(readings.bmeGas&&*readings.bmeGas>0&&*readings.bmeGas<=FLT_MAX){m.has_gas_resistance=true;m.gas_resistance=*readings.bmeGas;}
  if(readings.bmeIaq&&*readings.bmeIaq<=500){m.has_iaq=true;m.iaq=*readings.bmeIaq;}
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
  readings.oneWire.apply(m);
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  const auto &extra=readings.accessory;
#if defined(CONFIG_MT_RAK12035)
  if (extra.soil.valid) {
    m.has_soil_temperature = m.has_soil_moisture = true;
    m.soil_temperature = extra.soil.celsius;
    m.soil_moisture = extra.soil.moisture;
  }
#endif
#if defined(CONFIG_MT_ADS1X15)
  extra.adc.apply(m);
#endif
  if(extra.hasRadiation&&extra.radiation>=0&&extra.radiation<=FLT_MAX){m.has_radiation=true;m.radiation=extra.radiation;}
  if(extra.hasRange&&extra.rangeMm>=0&&extra.rangeMm<=4500){m.has_distance=true;m.distance=extra.rangeMm;}
#if defined(CONFIG_MT_LARK_WEATHER)
  if(extra.hasWeather){
    applyWind(m,true,extra.wind,true,extra.bearing);
  }
#endif
  if(extra.hasRain&&extra.rainHour>=0&&extra.rainHour<=FLT_MAX&&extra.rainDay>=0&&extra.rainDay<=FLT_MAX){
    m.has_rainfall_1h=m.has_rainfall_24h=true;m.rainfall_1h=extra.rainHour;m.rainfall_24h=extra.rainDay;
  }
  using mt::stock::telemetry::LuxSource;
#if defined(CONFIG_MT_NATIVE_LUX)
  auto allowLux=[&](LuxSource source){return source>=readings.channels.luxSource;};
#else
  auto allowLux=[](LuxSource){return true;};
#endif
  if(extra.hasLight&&extra.lux>=0&&extra.lux<=FLT_MAX&&extra.uvIndex>=0&&extra.uvIndex<=FLT_MAX){
    m.has_uv_lux=true;m.uv_lux=extra.uvIndex;
    if(allowLux(LuxSource::Ltr390)){m.has_lux=true;m.lux=extra.lux;}
  }
#if defined(CONFIG_MT_VEML7700)
  // In stock registration order, VEML follows LTR390. Keep its UV reading.
  extra.optical.apply(m,allowLux(LuxSource::Veml7700));
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
  if(extra.hasTslLux && extra.tslLux>=0 && extra.tslLux<=FLT_MAX &&
     allowLux(extra.hasTsl2561Lux ? LuxSource::Tsl2561 : LuxSource::Tsl2591)){m.has_lux=true;m.lux=extra.tslLux;}
#endif
#if defined(CONFIG_MT_OPT3001)
  // Stock registers OPT3001 between TSL2591 and TSL2561. BH1750 follows.
  if(extra.hasOptLux && extra.optLux>=0 && extra.optLux<=FLT_MAX && allowLux(LuxSource::Opt3001)
#if defined(CONFIG_MT_TSL_LIGHT)
     && !extra.hasTsl2561Lux
#endif
  ){m.has_lux=true;m.lux=extra.optLux;}
#endif
#if defined(CONFIG_MT_BH1750)
  if(allowLux(LuxSource::Bh1750) && extra.hasBhLux && extra.bhLux>=0 && extra.bhLux<=FLT_MAX){m.has_lux=true;m.lux=extra.bhLux;}
#endif
  if(extra.hasWeight&&extra.weight>=0&&extra.weight<=FLT_MAX){m.has_weight=true;m.weight=extra.weight;}
  if(extra.hasStrikes){m.has_lightning_strike_count_1h=true;m.lightning_strike_count_1h=extra.strikes;}
  if(extra.hasStrikes&&extra.strikes&&extra.hasDistance&&extra.distance<63){
    m.has_lightning_distance_km=true;m.lightning_distance_km=extra.distance;
  }
#endif
  return m;
}

inline bool hasEnvironmentMetrics(const meshtastic_EnvironmentMetrics &m) {
  return
#if defined(CONFIG_MT_VEML7700)
    m.has_lux || m.has_white_lux ||
#endif
#if defined(CONFIG_MT_TSL_LIGHT) || defined(CONFIG_MT_BH1750) || defined(CONFIG_MT_OPT3001)
    m.has_lux ||
#endif
#if defined(CONFIG_MT_BME680_IAQ)
    m.has_gas_resistance||m.has_iaq||
#endif
#if defined(CONFIG_MT_RAK12035)
    m.has_soil_temperature || m.has_soil_moisture ||
#endif
#if defined(CONFIG_MT_ADS1X15)
    mt::stock::telemetry::accessory::AdcVoltages::any(m) ||
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
    #if defined(CONFIG_MT_LARK_WEATHER)
    m.has_wind_speed || m.has_wind_direction ||
#endif
    m.has_weight || m.has_lightning_strike_count_1h || m.has_radiation || m.has_distance || m.has_uv_lux || m.has_rainfall_1h ||
#endif
    m.has_temperature || m.has_relative_humidity || m.has_barometric_pressure
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || defined(CONFIG_MT_STOCK_INA_METRICS)
    || m.has_voltage || m.has_current
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS)
    || m.has_gas_resistance || m.has_lux || m.has_ir_lux || m.has_distance
#endif
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
    || mt::stock::telemetry::OneWireMeasurements::any(m)
#endif
    ;
}
}  // namespace zt::telemetry
