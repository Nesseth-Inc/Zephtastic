// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <float.h>
#include <meshtastic/telemetry.pb.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#if defined(CONFIG_MT_SCD4X_ADMIN)
#include "ZephyrScd4xAdmin.h"
#endif
namespace mt::stock::telemetry {
// One fetch gives a coherent sample. SCD4x and Plantower drivers implement
// SENSOR_CHAN_ALL on fetch, then expose individual channels through get.
inline bool readAirChannels(const device *dev,meshtastic_AirQualityMetrics &m){
    if(!dev||!device_is_ready(dev))return false;
#if defined(CONFIG_MT_SCD4X_ADMIN)
    if(!scd4x::lockSample(dev))return false;
    struct SampleLock {const device *dev;~SampleLock(){scd4x::unlockSample(dev);}} lock{dev};
#endif
    if(sensor_sample_fetch(dev))return false;
    auto uintField=[dev](sensor_channel chan,bool &has,uint32_t &value){
        sensor_value v{};
        if(sensor_channel_get(dev,chan,&v))return false;
        const double measured=sensor_value_to_double(&v);
        if(!(measured>=0 && measured<=UINT32_MAX))return false;
        // Round fractional concentrations without overflowing at UINT32_MAX.
        value=uint32_t(measured);
        if(value<UINT32_MAX && measured-value>=0.5)++value;
        has=true;return true;
    };
    auto floatField=[dev](sensor_channel chan,bool &has,float &value,bool percentage){
        sensor_value v{};
        if(sensor_channel_get(dev,chan,&v))return;
        const float measured=sensor_value_to_double(&v);
        if(!(measured>=-FLT_MAX && measured<=FLT_MAX) || (percentage&&(measured<0||measured>100)))return;
        has=true;value=measured;
    };
    bool pm=false;
    pm|=uintField(SENSOR_CHAN_PM_1_0_CF,m.has_pm10_standard,m.pm10_standard);
    pm|=uintField(SENSOR_CHAN_PM_2_5_CF,m.has_pm25_standard,m.pm25_standard);
    pm|=uintField(SENSOR_CHAN_PM_10_CF,m.has_pm100_standard,m.pm100_standard);
    pm|=uintField(SENSOR_CHAN_PM_1_0,m.has_pm10_environmental,m.pm10_environmental);
    pm|=uintField(SENSOR_CHAN_PM_2_5,m.has_pm25_environmental,m.pm25_environmental);
    pm|=uintField(SENSOR_CHAN_PM_10,m.has_pm100_environmental,m.pm100_environmental);
    pm|=uintField(SENSOR_CHAN_PM_0_3_COUNT,m.has_particles_03um,m.particles_03um);
    pm|=uintField(SENSOR_CHAN_PM_0_5_COUNT,m.has_particles_05um,m.particles_05um);
    pm|=uintField(SENSOR_CHAN_PM_1_0_COUNT,m.has_particles_10um,m.particles_10um);
    pm|=uintField(SENSOR_CHAN_PM_2_5_COUNT,m.has_particles_25um,m.particles_25um);
    pm|=uintField(SENSOR_CHAN_PM_5_COUNT,m.has_particles_50um,m.particles_50um);
    pm|=uintField(SENSOR_CHAN_PM_10_COUNT,m.has_particles_100um,m.particles_100um);
    const bool co2=uintField(SENSOR_CHAN_CO2,m.has_co2,m.co2);
    if(co2){
        floatField(SENSOR_CHAN_AMBIENT_TEMP,m.has_co2_temperature,m.co2_temperature,false);
        floatField(SENSOR_CHAN_HUMIDITY,m.has_co2_humidity,m.co2_humidity,true);
    }
    if(pm){
        floatField(SENSOR_CHAN_AMBIENT_TEMP,m.has_pm_temperature,m.pm_temperature,false);
        floatField(SENSOR_CHAN_HUMIDITY,m.has_pm_humidity,m.pm_humidity,true);
    }
    // SENSOR_CHAN_VOC is a concentration in ppb. It cannot populate the
    // dimensionless pm_voc_idx field of the Sensirion PM sensor.
    return pm||co2;
}
}
