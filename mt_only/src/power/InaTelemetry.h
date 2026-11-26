// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PowerDonorReading.h"
#include "stock/telemetry/EnvironmentChannels.h"

namespace mt::power {
// Upstream 8e6a88d queries INA219, INA226, INA260, then INA3221.
// Each later successful reading wins only the fields the chip actually supplies.
#if defined(CONFIG_MT_INA260)
// The Zephyr API uses floating SI volts/amps. Recover the identified chip's
// exact 1.25 mV/mA ADC codes before reproducing the stock wire operations.
// Multiplying the SI float by 1000 directly can differ by one binary32 ULP.
inline zt::telemetry::PowerChannel ina260Channel(const env_sensor_reading &raw){
    zt::telemetry::PowerChannel value{};
    if((raw.fields&ENV_F_VOLTAGE)&&raw.voltage_v>=0&&raw.voltage_v<82){
        const unsigned code=unsigned(raw.voltage_v*800.0f+0.5f);
        if(code<=UINT16_MAX)value.voltage=(code*1.25f)/1000.0f;
    }
    if((raw.fields&ENV_F_CURRENT)&&raw.current_a>=-41&&raw.current_a<=41){
        const float code=raw.current_a*800.0f;
        const int count=int(code+(code<0?-0.5f:0.5f));
        if(count>=INT16_MIN&&count<=INT16_MAX)value.current=count*1.25f;
    }
    return value;
}
#endif
struct InaTelemetry {
    uint8_t entries[ENV_INA3221_CH3]{};
    void record(env_ina_source source,unsigned index){entries[source-1]=uint8_t(index+1);}
    template<class Read>
    void sample(bool environment,bool power,unsigned envChannel,Read read,
                stock::telemetry::EnvironmentChannels &env,zt::telemetry::PowerReadings &out) const {
        using Field=stock::telemetry::EnvironmentChannels;
        for(unsigned s=ENV_INA219;s<=ENV_INA3221_CH3;++s){
            if(!entries[s-1])continue;
            const bool triple=s>=ENV_INA3221_CH1;
            const unsigned channel=triple?s-ENV_INA3221_CH1:s==ENV_INA226?0:2;
            // EnvironmentTelemetry does not call INA226::getEnvironmentMetrics.
            const bool include=environment && s!=ENV_INA226 && (!triple||channel+1==envChannel);
            if(!include&&!power)continue;
            env_sensor_reading raw{};
            if(read(entries[s-1]-1,&raw))continue;
            const auto value=
#if defined(CONFIG_MT_INA260)
                s==ENV_INA260?ina260Channel(raw):
#endif
                zt::telemetry::measuredPowerChannel(raw);
            if(include){
                if(value.voltage)env.record(Field::Voltage,*value.voltage);
                if(value.current)env.record(Field::CurrentMa,*value.current);
            }
            if(power){
                // INA219/260 write EnvironmentMetrics on a PowerMetrics request
                // in stock. Those fields encode as the third power channel.
                if(value.voltage)out.voltage[channel]=value.voltage;
                if(value.current)out.current[channel]=value.current;
            }
        }
    }
};
}
