// SPDX-License-Identifier: GPL-3.0-only
#include "environment/ZephyrEnvironment.h"
#include "power/PowerMetricsAdapter.h"
#include "ZephyrEnvSensors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <pb_encode.h>
namespace {
struct Sample {float voltage,current;};
Sample rows[]={{},{3.5f,125},{4.25f,-250},{4.0f,375},{8,500},{9,-625},{10,750}};
unsigned present=0;
bool enabled(unsigned s){return present&(1u<<(s-1));}
struct Chip {
    unsigned id;
    bool hasSensor()const{return enabled(id);}
    float getBusVoltage_V(){return rows[id].voltage;}
    float getCurrent_mA(){return rows[id].current;}
    float getBusVoltage(){return rows[id].voltage;}
    float readBusVoltage(){return rows[id].voltage*1000;}
    float readCurrent(){return rows[id].current;}
};
struct INA219Sensor:Chip{Chip &ina219=*this;bool getMetrics(meshtastic_Telemetry*);} ina219Sensor{{1}};
struct INA226Sensor:Chip{Chip &ina226=*this;bool getMetrics(meshtastic_Telemetry*);bool getEnvironmentMetrics(meshtastic_Telemetry*);bool getPowerMetrics(meshtastic_Telemetry*);} ina226Sensor{{2}};
struct INA260Sensor:Chip{Chip &ina260=*this;bool getMetrics(meshtastic_Telemetry*);} ina260Sensor{{3}};
struct _INA3221Measurement {float voltage,current;};
struct _INA3221Measurements {_INA3221Measurement measurements[3];};
struct INA3221Sensor:Chip{
    _INA3221Measurement getMeasurement(unsigned ch){return {rows[4+ch].voltage,rows[4+ch].current};}
    _INA3221Measurements getMeasurements(){return {{getMeasurement(0),getMeasurement(1),getMeasurement(2)}};}
    bool getMetrics(meshtastic_Telemetry*);bool getEnvironmentMetrics(meshtastic_Telemetry*);bool getPowerMetrics(meshtastic_Telemetry*);
} ina3221Sensor{{4}};
#define ENV_CH (CONFIG_MT_INA3221_ENV_CHANNEL-1)
#define INA219_MULTIPLIER 1.0f
#include "methods.inc"
unsigned listed[6],count,calls,fields[7];bool failures[7];
}
extern "C" int env_sensors_init(){return 0;}
extern "C" int env_sensor_count(){return count;}
extern "C" bool env_sensor_is_power(int){return true;}
extern "C" env_ina_source env_sensor_ina_source(int i){return static_cast<env_ina_source>(listed[i]);}
extern "C" int env_sensor_read_metrics(int index,env_sensor_reading *out){
    ++calls;const auto s=listed[index];if(failures[s])return -1;
    out->fields=fields[s];out->voltage_v=rows[s].voltage;out->current_a=rows[s].current/1000.f;return 0;
}
static std::vector<uint8_t> wire(const meshtastic_Telemetry &m){
    uint8_t bytes[240];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&stream,meshtastic_Telemetry_fields,&m));return {bytes,bytes+stream.bytes_written};
}
int main(){
    static_assert(offsetof(meshtastic_EnvironmentMetrics,voltage)==offsetof(meshtastic_PowerMetrics,ch3_voltage));
    static_assert(offsetof(meshtastic_EnvironmentMetrics,current)==offsetof(meshtastic_PowerMetrics,ch3_current));
    uint64_t now=0;
    for(auto &f:fields)f=ENV_F_VOLTAGE|ENV_F_CURRENT;
#ifdef CONFIG_MT_SINGLE_INA219
    constexpr unsigned combinations=2;
#else
    constexpr unsigned combinations=16;
#endif
    for(present=0;present<combinations;++present){
        count=0;for(unsigned s:{4u,5u,6u,1u,3u,2u})if(enabled(s>4?4:s))listed[count++]=s;
        for(unsigned modes=0;modes<=3;++modes){
            calls=0;now+=2000;mt::environment::poll(modes&1,modes&2,now);
            meshtastic_Telemetry donor{},actual{};
            donor.which_variant=actual.which_variant=meshtastic_Telemetry_environment_metrics_tag;
            if(modes&1)donorEnvironment(&donor);
            actual.variant.environment_metrics=zt::telemetry::toMeshtasticEnvironmentMetrics(mt::environment::snapshot(now));assert(wire(actual)==wire(donor));
            donor={};actual={};donor.which_variant=actual.which_variant=meshtastic_Telemetry_power_metrics_tag;
            if(modes&2)donorPower(&donor);
            auto power=mt::environment::powerSnapshot(now);actual.variant.power_metrics=zt::telemetry::buildPowerMetrics(power.voltage,power.current);assert(wire(actual)==wire(donor));
            assert(calls<=count);if(!modes)assert(!calls);
        }
    }
#if defined(CONFIG_MT_SINGLE_INA219)
    puts("PASS T096 single INA219 source matches the full stock producer for all enable states");
    return 0;
#endif
    // A missing measurement must not erase a lower-priority valid field.
    present=15;count=6;
    failures[3]=true;fields[6]=ENV_F_CURRENT;
    now+=2000;mt::environment::poll(true,true,now);
    auto power=mt::environment::powerSnapshot(now);
    assert(*power.voltage[2]==rows[1].voltage&&*power.current[2]==rows[6].current);
    puts("PASS 16 accessory combinations x independent enable modes match pinned Environment and Power wire; partial-failure precedence, one fetch per channel");
}
