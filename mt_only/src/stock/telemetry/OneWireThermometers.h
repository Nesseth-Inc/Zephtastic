// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <float.h>
#include <errno.h>
#include <meshtastic/telemetry.pb.h>
#include <meshtastic/admin.pb.h>
namespace mt::stock::telemetry {
// DS2482-800 maintains a fixed slot for each of its eight one-wire buses.
// The stock volatile mainTemperatureChannel starts at zero after boot.
struct OneWireMeasurements {
    uint8_t mask=0;
    float celsius[8]{};
    void record(unsigned slot,float temperature){
        if(slot>=8||!(temperature>=-FLT_MAX&&temperature<=FLT_MAX))return;
        celsius[slot]=temperature;mask|=uint8_t(1u<<slot);
    }
    static constexpr bool meshtastic_EnvironmentMetrics::* presence[8]={
        &meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch0,&meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch1,
        &meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch2,&meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch3,
        &meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch4,&meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch5,
        &meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch6,&meshtastic_EnvironmentMetrics::has_one_wire_temperature_ch7};
    static constexpr float meshtastic_EnvironmentMetrics::* values[8]={
        &meshtastic_EnvironmentMetrics::one_wire_temperature_ch0,&meshtastic_EnvironmentMetrics::one_wire_temperature_ch1,
        &meshtastic_EnvironmentMetrics::one_wire_temperature_ch2,&meshtastic_EnvironmentMetrics::one_wire_temperature_ch3,
        &meshtastic_EnvironmentMetrics::one_wire_temperature_ch4,&meshtastic_EnvironmentMetrics::one_wire_temperature_ch5,
        &meshtastic_EnvironmentMetrics::one_wire_temperature_ch6,&meshtastic_EnvironmentMetrics::one_wire_temperature_ch7};
    void apply(meshtastic_EnvironmentMetrics &out)const{
        for(unsigned i=0;i<8;++i)if(mask&(1u<<i)){out.*presence[i]=true;out.*values[i]=celsius[i];}
    }
    void from(const meshtastic_EnvironmentMetrics &m){
        for(unsigned i=0;i<8;++i)if(m.*presence[i])record(i,m.*values[i]);
    }
    static bool any(const meshtastic_EnvironmentMetrics &m){
        for(auto field:presence)if(m.*field)return true;
        return false;
    }
};
inline int oneWireSelection(const meshtastic_SensorConfig &cfg,unsigned &slot){
    if(!cfg.has_ds248x_config||cfg.has_scd4x_config||cfg.has_sen5x_config||cfg.has_scd30_config||
       cfg.has_shtxx_config||cfg.has_sen6x_config||cfg.has_as3935_config)return -ENOTSUP;
    if(!cfg.ds248x_config.has_main_temperature_channel||cfg.ds248x_config.main_temperature_channel>=8)return -EINVAL;
    slot=cfg.ds248x_config.main_temperature_channel;
    return 0;
}
namespace oneWire {
void poll(bool enabled,uint64_t now);
bool snapshot(uint64_t now,OneWireMeasurements &out,unsigned &primary);
int select(unsigned primary);
}
}
