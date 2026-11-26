// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrEnvironment.h"
#include "EnvironmentSnapshot.h"
#if defined(CONFIG_MT_MLX90632)
#include "stock/telemetry/mlx90632/ObjectTemperature.h"
#endif
#if defined(CONFIG_MT_BME680_IAQ)
#include "stock/telemetry/bme680/ZephyrBme680.h"
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
#include "stock/telemetry/accessory/Accessories.h"
#endif
#include "EnvironmentDonorReading.h"
#include "power/PowerSnapshot.h"
#if defined(CONFIG_MT_MAX17048)
#include "power/Max17048.h"
#endif
#include "power/PowerDonorReading.h"
#if defined(CONFIG_MT_STOCK_INA_METRICS) && !defined(CONFIG_MT_SINGLE_INA219)
#include "power/InaTelemetry.h"
#endif
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS)
#include "stock/telemetry/ZephyrEnvironmentChannels.h"
#endif
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) && defined(__ZEPHYR__)
#define MT_SENSOR_COMPAT(alias, compat) DT_NODE_HAS_COMPAT(DT_ALIAS(alias), compat)
#define MT_TEMPERATURE_ONLY(alias) (MT_SENSOR_COMPAT(alias, jedec_jc_42_4_temp) || MT_SENSOR_COMPAT(alias, lm75))
#define MT_ALIAS_ORDER(alias) ( \
  MT_SENSOR_COMPAT(alias, lm75) ? Source::Pct2075 : \
  MT_SENSOR_COMPAT(alias, jedec_jc_42_4_temp) ? Source::Mcp9808 : \
  MT_SENSOR_COMPAT(alias, bosch_bmp388) || MT_SENSOR_COMPAT(alias, bosch_bmp390) ? Source::Bmp3xx : \
  MT_SENSOR_COMPAT(alias, sensirion_sht21) || MT_SENSOR_COMPAT(alias, sensirion_sht3xd) || MT_SENSOR_COMPAT(alias, sensirion_sht4x) || MT_SENSOR_COMPAT(alias, sensirion_shtcx) ? Source::Sht : \
  MT_SENSOR_COMPAT(alias, aosong_aht20) || MT_SENSOR_COMPAT(alias, aosong_dht20) ? Source::Aht : \
  MT_SENSOR_COMPAT(alias, bosch_bme280) ? Source::Bme280 : \
  MT_SENSOR_COMPAT(alias, bosch_bme680) ? Source::Bme680 : \
  MT_SENSOR_COMPAT(alias, infineon_dps310) ? Source::Dps310 : \
  MT_SENSOR_COMPAT(alias, st_lps22hb_press) ? Source::Lps22hb : \
  MT_SENSOR_COMPAT(alias, goertek_spa06) ? Source::Spa06 : Source::Declared)
#else
#define MT_TEMPERATURE_ONLY(alias) false
#define MT_ALIAS_ORDER(alias) Source::Declared
#endif
namespace mt::environment {
using Source = zt::telemetry::EnvironmentSource;
namespace {
zt::telemetry::EnvironmentSnapshot latest;
zt::telemetry::PowerSnapshot latestPower;
#if defined(CONFIG_MT_MAX17048)
bool gaugePowerEnabled=false;
#endif
bool initialized=false,initializationReady=false,sampled=false;
uint64_t sampledAt=0;
}
void poll(bool environmentEnabled,bool powerEnabled,uint64_t now){
#if defined(CONFIG_MT_MLX90632)
  stock::telemetry::mlx90632::poll(environmentEnabled && now<=INT64_MAX,now);
#endif
#if defined(CONFIG_MT_MAX17048)
  gaugePowerEnabled=powerEnabled && now<=INT64_MAX;
#endif
#if defined(CONFIG_MT_BME680_IAQ)
  stock::telemetry::bme680::enable(environmentEnabled&&now<=INT64_MAX);
#endif
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
  stock::telemetry::oneWire::poll(environmentEnabled&&now<=INT64_MAX,now);
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  stock::telemetry::accessory::enableEnvironment(environmentEnabled&&now<=INT64_MAX);
#endif
  if(!environmentEnabled)latest.invalidate();
  if(!powerEnabled)latestPower.invalidate();
  if((!environmentEnabled&&!powerEnabled)||now>INT64_MAX){latest.invalidate();latestPower.invalidate();sampled=false;return;}
  if(sampled&&now<sampledAt){latest.invalidate();latestPower.invalidate();sampled=false;return;}
  if(sampled&&now-sampledAt<1000)return;
  if(!initialized){initialized=true;initializationReady=env_sensors_init()==0;}
  if(!initializationReady){latest.invalidate();latestPower.invalidate();return;}
  sampled=true;sampledAt=now;
  zt::telemetry::EnvironmentReadings merged;
  zt::telemetry::PowerReadings power;
#if defined(CONFIG_MT_STOCK_INA_METRICS) && !defined(CONFIG_MT_SINGLE_INA219)
  mt::power::InaTelemetry ina;
#endif
  const int count=env_sensor_count();
#if !defined(CONFIG_MT_SINGLE_INA219)
  unsigned powerChannel=0;
#endif
  for(int i=0;i<count&&i<ENV_SENSOR_LIMIT;++i){
    const bool isPower=env_sensor_is_power(i);
    // Native classification assigns stable channels before any fetch. A bad
    // sample leaves a hole; a later monitor cannot shift into that channel.
#if defined(CONFIG_MT_SINGLE_INA219)
    // The only native power source on this board. Its stock wire slot is CH3.
    constexpr unsigned channel=2;
#else
    const unsigned channel=isPower?powerChannel++:0;
#endif
    if(isPower) {
#if defined(CONFIG_MT_STOCK_INA_METRICS) && !defined(CONFIG_MT_SINGLE_INA219)
      const auto source=env_sensor_ina_source(i);
      if(source!=ENV_INA_NONE){ina.record(source,i);continue;}
#endif
#if defined(CONFIG_MT_SINGLE_INA219)
      // Reaching this loop already requires at least one enabled producer.
#elif defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS)
      if((!environmentEnabled&&!powerEnabled)||channel>=8)continue;
#else
      if(!powerEnabled||channel>=8)continue;
#endif
    } else if(!environmentEnabled)continue;
    env_sensor_reading raw{};if(env_sensor_read_metrics(i,&raw))continue;
    if(isPower){
      (void)zt::telemetry::assignPowerChannel(power,channel,raw);
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS) || defined(CONFIG_MT_SINGLE_INA219)
      using Field=mt::stock::telemetry::EnvironmentChannels;
      if(environmentEnabled){
        if(power.voltage[channel])merged.channels.record(Field::Voltage,*power.voltage[channel]);
        if(power.current[channel])merged.channels.record(Field::CurrentMa,*power.current[channel]);
      }
#endif
      continue;
    }
    const auto source = static_cast<Source>(raw.environment_source);
    if ((raw.fields & ENV_F_TEMPERATURE) && raw.temperature_c >= -FLT_MAX && raw.temperature_c <= FLT_MAX)
      merged.ambientCelsius.record(source, raw.temperature_c);
    if ((raw.fields & ENV_F_HUMIDITY) && raw.humidity_pct >= 0 && raw.humidity_pct <= 100)
      merged.humidityPercent.record(source, raw.humidity_pct);
    if ((raw.fields & ENV_F_PRESSURE) && raw.pressure_hpa > 0 && raw.pressure_hpa <= FLT_MAX) {
      merged.pressureHpa.record(source, raw.pressure_hpa);
      if (source == Source::Bmp3xx) merged.humidityPercent.clear(source);
    }
  }
#if defined(CONFIG_MT_STOCK_INA_METRICS) && !defined(CONFIG_MT_SINGLE_INA219)
#if defined(CONFIG_MT_INA3221_ENV_CHANNEL)
  constexpr unsigned envChannel=CONFIG_MT_INA3221_ENV_CHANNEL;
#else
  constexpr unsigned envChannel=1;
#endif
  ina.sample(environmentEnabled,powerEnabled,envChannel,env_sensor_read_metrics,merged.channels,power);
#endif
#if defined(CONFIG_MT_STOCK_ENVIRONMENT_CHANNELS)
  if(environmentEnabled){
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_environment),okay)
    stock::telemetry::readEnvironmentChannels<MT_TEMPERATURE_ONLY(zt_environment), MT_ALIAS_ORDER(zt_environment)>(DEVICE_DT_GET(DT_ALIAS(zt_environment)),merged);
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_environment_2),okay)
    stock::telemetry::readEnvironmentChannels<MT_TEMPERATURE_ONLY(zt_environment_2), MT_ALIAS_ORDER(zt_environment_2)>(DEVICE_DT_GET(DT_ALIAS(zt_environment_2)),merged);
#endif
  }
#endif
  if(environmentEnabled)latest.publish(merged,now);
  if(powerEnabled)latestPower.publish(power,now);
}
zt::telemetry::EnvironmentReadings snapshot(uint64_t now){
  zt::telemetry::EnvironmentReadings out;(void)latest.snapshot(now,90000,out);
#if defined(CONFIG_MT_ONE_WIRE_TELEMETRY)
  unsigned primary=0;
  if(stock::telemetry::oneWire::snapshot(now,out.oneWire,primary)&&(out.oneWire.mask&(1u<<primary)))
    out.ambientCelsius.record(zt::telemetry::EnvironmentSource::OneWire, out.oneWire.celsius[primary]);
#endif
#if defined(CONFIG_MT_ACCESSORY_SENSORS)
  out.accessory=stock::telemetry::accessory::environment(now);
#endif
#if defined(CONFIG_MT_BME680_IAQ)
  stock::telemetry::bme680::merge(out,now);
#endif
#if defined(CONFIG_MT_MLX90632)
  const auto object=stock::telemetry::mlx90632::snapshot(now);
  if (object) out.ambientCelsius.record(zt::telemetry::EnvironmentSource::Mlx90632, *object);
#endif
  return out;
}
zt::telemetry::PowerReadings powerSnapshot(uint64_t now){
  zt::telemetry::PowerReadings out;(void)latestPower.snapshot(now,90000,out);
#if defined(CONFIG_MT_MAX17048)
  if(gaugePowerEnabled)power::max17048::snapshot(now).power(out);
#endif
  return out;
}
}
