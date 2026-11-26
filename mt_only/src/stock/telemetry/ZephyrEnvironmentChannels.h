// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "environment/EnvironmentMetricsAdapter.h"
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/devicetree.h>
#if defined(CONFIG_MT_SCD4X_ADMIN)
#include "ZephyrScd4xAdmin.h"
#endif
namespace mt::stock::telemetry {
#if defined(CONFIG_MT_NATIVE_LUX)
inline LuxSource nativeLuxSource(const device *dev) {
#define ZT_LUX_SOURCE(node) (DT_NODE_HAS_COMPAT(node,ams_tsl2591) ? LuxSource::Tsl2591 : \
    DT_NODE_HAS_COMPAT(node,ti_opt3001) ? LuxSource::Opt3001 : \
    DT_NODE_HAS_COMPAT(node,ams_tsl2561) ? LuxSource::Tsl2561 : \
    DT_NODE_HAS_COMPAT(node,rohm_bh1750) ? LuxSource::Bh1750 : \
    DT_NODE_HAS_COMPAT(node,vishay_veml7700) ? LuxSource::Veml7700 : LuxSource::Declared)
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_environment),okay)
    if(dev==DEVICE_DT_GET(DT_ALIAS(zt_environment))) return ZT_LUX_SOURCE(DT_ALIAS(zt_environment));
#endif
#if DT_NODE_HAS_STATUS(DT_ALIAS(zt_environment_2),okay)
    if(dev==DEVICE_DT_GET(DT_ALIAS(zt_environment_2))) return ZT_LUX_SOURCE(DT_ALIAS(zt_environment_2));
#endif
#undef ZT_LUX_SOURCE
    return LuxSource::Declared;
}
#endif
template<bool TemperatureOnly=false, zt::telemetry::EnvironmentSource Source=zt::telemetry::EnvironmentSource::Declared>
inline void readEnvironmentChannels(const device *dev,zt::telemetry::EnvironmentReadings &out){
#if defined(CONFIG_MT_BME680_IAQ)
    if(dev==DEVICE_DT_GET(DT_ALIAS(zt_bme680)))return;
#endif

    if(!dev || !device_is_ready(dev))return;
#if defined(CONFIG_MT_SCD4X_ADMIN)
    if(!scd4x::lockSample(dev))return;
    struct SampleLock {const device *dev;~SampleLock(){scd4x::unlockSample(dev);}} lock{dev};
#endif
    if(sensor_sample_fetch(dev))return;
    sensor_value sample{};
    auto get=[&](sensor_channel channel,float &v){
        if(sensor_channel_get(dev,channel,&sample)) return false;
        v=sensor_value_to_float(&sample);
        return v>=-FLT_MAX && v<=FLT_MAX;
    };
    float v=0;
#if defined(CONFIG_MT_NATIVE_LUX)
    const auto source=nativeLuxSource(dev);
    if (source!=LuxSource::Declared) {
        // TSL2591 combines visible and IR into lux with ALL. The other native
        // photometers report lux via LIGHT; raw VEML white counts are not lux.
        if (get(source==LuxSource::Tsl2591 ? SENSOR_CHAN_ALL : SENSOR_CHAN_LIGHT,v))
            out.channels.recordLux(source,v);
        return;
    }
#endif
    if(get(SENSOR_CHAN_AMBIENT_TEMP,v)) out.ambientCelsius.record(Source,v);
    // The JEDEC thermometer driver asserts on unsupported channel requests.
    if constexpr(TemperatureOnly)return;
    if(get(SENSOR_CHAN_HUMIDITY,v)&&v>=0&&v<=100) out.humidityPercent.record(Source,v);
    if (get(SENSOR_CHAN_PRESS,v) && v>0 && v<=FLT_MAX/10) {
        out.pressureHpa.record(Source,v*10);
        if constexpr (Source == zt::telemetry::EnvironmentSource::Bmp3xx) out.humidityPercent.clear(Source);
    }
    if(get(SENSOR_CHAN_GAS_RES,v)) out.channels.record(EnvironmentChannels::GasKohms,v/1000);
    if(get(SENSOR_CHAN_VOLTAGE,v)) out.channels.record(EnvironmentChannels::Voltage,v);
    if(get(SENSOR_CHAN_CURRENT,v)) out.channels.record(EnvironmentChannels::CurrentMa,v*1000);
    if(get(SENSOR_CHAN_LIGHT,v)||get(SENSOR_CHAN_AMBIENT_LIGHT,v)) {
#if defined(CONFIG_MT_NATIVE_LUX)
        out.channels.recordLux(LuxSource::Declared,v);
#else
        out.channels.record(EnvironmentChannels::Lux,v);
#endif
    }
    if(get(SENSOR_CHAN_IR,v)) out.channels.record(EnvironmentChannels::IrLux,v);
    if(get(SENSOR_CHAN_DISTANCE,v)) out.channels.record(EnvironmentChannels::DistanceMm,v*1000);
}
}
