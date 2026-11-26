// SPDX-License-Identifier: GPL-3.0-only
#include "environment/ZephyrEnvironment.cpp"
#include "telemetry/CompletionTelemetry.h"
#include "EnvironmentQuery.h"
#include "ModuleAnnouncement.h"
#include "stock/telemetry/ZephyrScd4xAdmin.h"
#include <zephyr/kernel.h>
extern "C" {
#include <zephyr/drivers/sensor/scd4x.h>
}
#include <cassert>
#include <map>
#include <cstdio>

std::map<int,double> attrs;
int shtAccuracyLevel=2, shtError=0;
bool lowPower=false, baselineError=false;
int writes=0, modeChanges=0, calibrationSampleError=0;
uint16_t baselinePpm=0;
extern "C" int scd4x_set_periodic_mode(const device *d,bool low){
    assert(d==&zt_scd4x);++modeChanges;lowPower=low;return 0;
}
extern "C" int scd4x_set_asc_target(const device *d,uint16_t ppm){
    assert(d==&zt_scd4x);++writes;if(baselineError)return -EIO;baselinePpm=ppm;return 0;
}
extern "C" int scd4x_fetch_calibration_sample(const device *d){
    return calibrationSampleError?calibrationSampleError:sensor_sample_fetch(d);
}
bool frcError=false;int forced=0,resets=0,persists=0;
static std::map<int,double> channels[7];
static unsigned reads[7];
bool device_is_ready(const device *d){return d!=nullptr;}
int opticalFetch();int opticalGet(sensor_channel,sensor_value *);
int sensor_sample_fetch(const device *d){++reads[d->id];return d->id==6?opticalFetch():0;}
int sensor_channel_get(const device *d,sensor_channel c,sensor_value *v){
    if(d->id==6)return opticalGet(c,v);
    auto it=channels[d->id].find(c);
    return it==channels[d->id].end()?-ENOTSUP:sensor_value_from_float(v,it->second);
}
int sensor_attr_get(const device *d,sensor_channel,sensor_attribute a,sensor_value *v){
    assert(d==&zt_scd4x);return sensor_value_from_float(v,attrs[a]);
}
int sensor_attr_set(const device *d,sensor_channel,sensor_attribute a,const sensor_value *v){
    if(d==&zt_shtxx){
        assert(a==SENSOR_ATTR_RESOLUTION);if(shtError)return shtError;shtAccuracyLevel=v->val1;return 0;
    }
    assert(d==&zt_scd4x);attrs[a]=sensor_value_to_double(v);return 0;
}
extern "C" int scd4x_factory_reset(const device *){++resets;return 0;}
extern "C" int scd4x_persist_settings(const device *){++persists;return 0;}
extern "C" int scd4x_forced_recalibration(const device *,uint16_t target,uint16_t *corr){
    assert(target==435);++forced;*corr=142;return frcError?-EIO:0;
}
extern "C" int env_sensors_init(){return 0;}
extern "C" int env_sensor_count(){return 0;}
extern "C" bool env_sensor_is_power(int){return false;}
extern "C" int env_sensor_read_metrics(int,env_sensor_reading *){assert(false);return -EIO;}
static meshtastic_Telemetry extract(const uint8_t *wire,size_t n,uint32_t id){
    meshtastic_Data d{};auto i=pb_istream_from_buffer(wire,n);
    assert(pb_decode(&i,meshtastic_Data_fields,&d)&&d.request_id==id);
    meshtastic_Telemetry t{};i=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&i,meshtastic_Telemetry_fields,&t));return t;
}
void adminCases();void opticalCases();
int main(){
    using mt::telemetry::CompletionTelemetry;
    channels[1]={{SENSOR_CHAN_PM_1_0_CF,11},{SENSOR_CHAN_PM_2_5_CF,22},{SENSOR_CHAN_PM_10_CF,33},
        {SENSOR_CHAN_PM_1_0,44},{SENSOR_CHAN_PM_2_5,55},{SENSOR_CHAN_PM_10,66},{SENSOR_CHAN_PM_0_3_COUNT,789},
        {SENSOR_CHAN_PM_10_COUNT,4},{SENSOR_CHAN_VOC,99}};
    channels[2]={{SENSOR_CHAN_CO2,434},{SENSOR_CHAN_HUMIDITY,42},{SENSOR_CHAN_AMBIENT_TEMP,23.25}};
    channels[3]={{SENSOR_CHAN_AMBIENT_TEMP,35.5}};
    channels[4]={{SENSOR_CHAN_LIGHT,0},{SENSOR_CHAN_IR,15.25},{SENSOR_CHAN_DISTANCE,1.5},
        {SENSOR_CHAN_CURRENT,-0.125},{SENSOR_CHAN_VOLTAGE,0},{SENSOR_CHAN_GAS_RES,124000}};
    CompletionTelemetry completion;meshtastic_ModuleConfig_TelemetryConfig settings{};
    settings.air_quality_enabled=settings.health_measurement_enabled=true;
    assert(completion.configSupported(settings)&&completion.configure(settings)&&completion.poll(200)==CompletionTelemetry::Result::Ok);
    CompletionTelemetry::Snapshot s{};assert(completion.snapshot(201,s)==CompletionTelemetry::Result::Ok);
    assert(s.air.pm10_standard==11 && s.air.pm10_environmental==44 && s.air.pm25_standard==22&&s.air.pm25_environmental==55);
    assert(s.air.pm100_standard==33&&s.air.pm100_environmental==66&&s.air.particles_03um==789&&s.air.particles_100um==4);
    assert(s.air.co2==434&&s.air.co2_humidity==42&&s.air.co2_temperature==23.25f&&!s.air.has_pm_humidity&&!s.air.has_pm_voc_idx);
    assert(s.health.temperature==35.5f&&!s.health.has_heart_bpm);assert(reads[1]==1&&reads[2]==1&&reads[3]==1);
    uint8_t buf[239];size_t n=0;
    for(auto tag:{meshtastic_Telemetry_health_metrics_tag,meshtastic_Telemetry_air_quality_metrics_tag}){
        assert(completion.response(tag,42,201,100,buf,sizeof(buf),n)==CompletionTelemetry::Result::Ok);
        assert(extract(buf,n,42).which_variant==uint32_t(tag));
    }
    mt::environment::poll(true,false,1000);
    const auto sample=mt::environment::snapshot(1001);
    meshtastic_Telemetry report{};
    assert(mt::stock::telemetry::environmentReport(report,123,sample));
    const auto &m=report.variant.environment_metrics;
    assert(!m.has_temperature && m.has_lux && m.lux==0 && m.ir_lux==15.25 && m.distance==1500 &&
           m.has_voltage&&m.voltage==0&&m.current==-125&&m.gas_resistance==124);
    bool isTelemetry=false;
    assert(mt::stock::telemetry::encodeReport(report,0,buf,sizeof(buf),n)&&mt::ModuleAnnouncement::canonical(buf,n,isTelemetry)&&isTelemetry);
    assert(mt::EnvironmentQuery::responseMetrics(m,77,buf,sizeof(buf),n,123));
    const auto reply=extract(buf,n,77).variant.environment_metrics;assert(reply.has_ir_lux&&reply.has_voltage&&reply.distance==1500);
    assert(!mt::stock::telemetry::environmentReport(report,123,mt::environment::snapshot(100000)));
    puts("PASS multi-device air+health acquisition, PM reference and ambient mass/counts, CO2 companion fields, extra-only environment mesh/response, real units and absence");
    channels[5]={{SENSOR_CHAN_AMBIENT_TEMP,-4.25}};
    try{mockKernel::oneWireThread(nullptr,nullptr,nullptr);}catch(mockKernel::NoJob&){}
    auto wire=zt::telemetry::toMeshtasticEnvironmentMetrics(mt::environment::snapshot(1002));
    assert(wire.has_one_wire_temperature_ch6&&wire.one_wire_temperature_ch6==-4.25&&!wire.has_temperature&&!wire.has_one_wire_temperature_ch0);
    adminCases();
    wire=zt::telemetry::toMeshtasticEnvironmentMetrics(mt::environment::snapshot(1003));
    assert(wire.has_temperature&&wire.temperature==-4.25&&wire.has_one_wire_temperature_ch6);
    assert(mt::EnvironmentQuery::responseMetrics(wire,89,buf,sizeof(buf),n));
    assert(extract(buf,n,89).variant.environment_metrics.one_wire_temperature_ch6==-4.25);
    assert(!mt::environment::snapshot(91001).oneWire.mask);
    mt::environment::poll(false,false,1100);assert(!mt::environment::snapshot(1101).oneWire.mask);
    opticalCases();
    puts("PASS stock DS2482 fixed channel slots, canonical USB primary selection, measured temperature response and stale/disabled handling");
}
