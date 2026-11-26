// SPDX-License-Identifier: GPL-3.0-only
// Stock SCD4XSensor::handleAdminMessage and CO2Sensor::handleCo2AdminRequest.
#pragma once
#include <errno.h>
#include <meshtastic/admin.pb.h>

namespace mt::stock::telemetry {
inline int validateScd4x(const meshtastic_SensorConfig &s) {
    if(!s.has_scd4x_config||s.has_scd30_config||s.has_sen5x_config||s.has_sen6x_config||
       s.has_ds248x_config||s.has_shtxx_config||s.has_as3935_config)return -ENOTSUP;
    const auto &q=s.scd4x_config;
    if(q.has_factory_reset)return q.factory_reset?0:-EINVAL;
    if(q.has_set_target_co2_conc&&(!q.has_set_asc||!q.set_target_co2_conc||q.set_target_co2_conc>UINT16_MAX))return -EINVAL;
    if(q.has_set_asc&&!q.set_asc&&!q.has_set_target_co2_conc)return -EINVAL;
    if(q.has_set_altitude&&q.set_altitude>3000)return -EINVAL;
    if(!q.has_set_altitude&&q.has_set_ambient_pressure&&(q.set_ambient_pressure<70000||q.set_ambient_pressure>120000))return -EINVAL;
    if(q.has_set_temperature&&!(q.set_temperature>=-40&&q.set_temperature<=70))return -EINVAL;
    return q.has_set_altitude||q.has_set_ambient_pressure||q.has_set_asc||q.has_set_temperature||q.has_set_power_mode?0:-EINVAL;
}

template<class Driver> int applyScd4x(Driver &driver,const meshtastic_SCD4X_config &q){
    if(q.has_factory_reset)return driver.factoryReset();
    int error=0;
    if(q.has_set_asc){
        if(q.set_asc){
            error=driver.setAsc(true);
            if(!error&&q.has_set_target_co2_conc)
                error=driver.baseline(uint16_t(q.set_target_co2_conc));
        }
        else {
            bool previous=false;
            error=driver.getAsc(previous);
            if(!error)error=driver.setAsc(false);
            if(!error){
                error=driver.forcedCalibration(uint16_t(q.set_target_co2_conc));
                if(error)(void)driver.setAsc(previous);
            }
        }
    }
    if(!error&&q.has_set_altitude)error=driver.altitude(q.set_altitude);
    else if(!error&&q.has_set_ambient_pressure)error=driver.pressure(q.set_ambient_pressure);
    if(!error){
        if(q.has_set_temperature)error=driver.temperature(q.set_temperature);
        // Stock attempts an explicitly requested mode switch even if temperature failed.
        if(q.has_set_power_mode){
            const int modeError=driver.mode(q.set_power_mode);
            if(!error)error=modeError;
        }
    }
    return error;
}
}
