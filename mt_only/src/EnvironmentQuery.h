// SPDX-License-Identifier: GPL-3.0-only
// Canonical selector/encoder using the finite/range adapter.
// Caller owns the measured producer and reply admission.
#pragma once
#include "RemoteTelemetryQuery.h"
#include "environment/EnvironmentMetricsAdapter.h"
namespace mt {
class EnvironmentQuery {
public:
  static bool request(const uint8_t *payload,size_t size){
    return RemoteTelemetryQuery::requestVariant(payload,size)==meshtastic_Telemetry_environment_metrics_tag;
  }
  static bool responseMetrics(const meshtastic_EnvironmentMetrics &metrics,uint32_t request,
                              uint8_t *out,size_t capacity,size_t &size,uint32_t utc=0){
    size=0;if(!request||!out)return false;
    zt::telemetry::EnvironmentReadings readings;
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
#if defined(CONFIG_MT_ADS1X15)
    readings.accessory.adc.from(metrics);
#endif
#if defined(CONFIG_MT_RAK12035)
    if (metrics.has_soil_temperature && metrics.has_soil_moisture &&
        metrics.soil_temperature >= 0 && metrics.soil_temperature <= FLT_MAX)
      readings.accessory.soil = {true, metrics.soil_moisture, metrics.soil_temperature};
#endif
    readings.accessory.hasRadiation=metrics.has_radiation;readings.accessory.radiation=metrics.radiation;
    readings.accessory.hasRange=metrics.has_distance;readings.accessory.rangeMm=metrics.distance;
    readings.accessory.hasRain=metrics.has_rainfall_1h&&metrics.has_rainfall_24h;
    readings.accessory.rainHour=metrics.rainfall_1h;readings.accessory.rainDay=metrics.rainfall_24h;
    readings.accessory.hasLight=metrics.has_lux&&metrics.has_uv_lux;
    readings.accessory.lux=metrics.lux;readings.accessory.uvIndex=metrics.uv_lux;
#if defined(CONFIG_MT_VEML7700)
    readings.accessory.optical = {metrics.has_lux, metrics.has_white_lux, metrics.lux, metrics.white_lux};
#endif
#if defined(CONFIG_MT_TSL_LIGHT)
    readings.accessory.hasTslLux=metrics.has_lux;readings.accessory.tslLux=metrics.lux;
#endif
#if defined(CONFIG_MT_OPT3001)
    readings.accessory.hasOptLux=metrics.has_lux;readings.accessory.optLux=metrics.lux;
#endif
#if defined(CONFIG_MT_BH1750)
    readings.accessory.hasBhLux=metrics.has_lux;readings.accessory.bhLux=metrics.lux;
#endif
    readings.accessory.hasWeight=metrics.has_weight;readings.accessory.weight=metrics.weight;
    readings.accessory.hasStrikes=metrics.has_lightning_strike_count_1h;readings.accessory.strikes=metrics.lightning_strike_count_1h;
    const float km=metrics.lightning_distance_km;
    if(metrics.has_lightning_distance_km&&km>=0&&km<63&&float(uint8_t(km))==km){
      readings.accessory.hasDistance=true;readings.accessory.distance=uint8_t(km);
    }
#endif
#if defined(CONFIG_MT_BME680_IAQ)
    if(metrics.has_gas_resistance)readings.bmeGas=metrics.gas_resistance;
    if(metrics.has_iaq)readings.bmeIaq=metrics.iaq;
#endif
    if(metrics.has_temperature)readings.ambientCelsius=metrics.temperature;
    if(metrics.has_relative_humidity)readings.humidityPercent=metrics.relative_humidity;
    if(metrics.has_barometric_pressure)readings.pressureHpa=metrics.barometric_pressure;
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || defined(CONFIG_MT_STOCK_INA_METRICS)
    readings.channels.from(metrics);
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
    readings.oneWire.from(metrics);
#endif
    meshtastic_Telemetry report{};
    bool available=stock::telemetry::environmentReport(report,utc,readings);
#if defined(CONFIG_MT_VEML7700)
    if (metrics.has_uv_lux && metrics.uv_lux >= 0 && metrics.uv_lux <= FLT_MAX) {
      report.variant.environment_metrics.has_uv_lux = true;
      report.variant.environment_metrics.uv_lux = metrics.uv_lux;
      available = true;
    }
#endif
#if defined(CONFIG_MT_LARK_WEATHER)
    auto &outMetrics=report.variant.environment_metrics;
    zt::telemetry::applyWind(outMetrics,metrics.has_wind_speed,metrics.wind_speed,
                             metrics.has_wind_direction,metrics.wind_direction);
    available|=outMetrics.has_wind_speed||outMetrics.has_wind_direction;
#endif
    return available&&stock::telemetry::encodeReport(report,request,out,capacity,size);
  }
};
}
