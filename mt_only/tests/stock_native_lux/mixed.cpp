// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/ZephyrEnvironmentChannels.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace mt::stock::telemetry;
namespace {
constexpr unsigned models[]={NATIVE_FIRST,NATIVE_SECOND};
constexpr const device *devices[]={&zt_environment,&zt_environment_2};
bool failedFetch[2]{},failedChannel[2]{};
int values[2]{};
void consider(float &expected,unsigned &current,unsigned order,float value) {
    if(order>=current) {expected=value;current=order;}
}
void check(unsigned direct,bool directDark,unsigned reversed,unsigned failed,unsigned badChannel,unsigned dark) {
    zt::telemetry::EnvironmentReadings readings;
    auto &e=readings.accessory;
    e.hasLight=true;e.lux=90;e.uvIndex=1.25f;
    e.optical.hasWhite=true;e.optical.white=51;
    unsigned winner=1;float expected=90;
    for(unsigned i=0;i<2;i++) {
        failedFetch[i]=bool(failed&(1u<<i));
        failedChannel[i]=bool(badChannel&(1u<<i));
        values[i]=(dark&(1u<<i))?0:11+int(i);
        if(!failedFetch[i]&&!failedChannel[i])
            consider(expected,winner,models[i],values[i]);
    }
    readEnvironmentChannels(devices[reversed],readings);
    readEnvironmentChannels(devices[!reversed],readings);
    const float value=directDark?0:25;
    switch(direct) {
        case 0: break;
        case 2: e.optical.hasVisible=true;e.optical.visible=value;break;
        case 3: e.hasTslLux=true;e.tslLux=value;break;
        case 4: e.hasOptLux=true;e.optLux=value;break;
        case 5: e.hasTslLux=e.hasTsl2561Lux=true;e.tslLux=value;break;
        case 6: e.hasBhLux=true;e.bhLux=value;break;
        default: assert(false);
    }
    if(direct) consider(expected,winner,direct,value);
    meshtastic_Telemetry report{};assert(environmentReport(report,9,readings));
    auto verify=[&](const meshtastic_EnvironmentMetrics &metrics) {
        assert(metrics.has_lux && metrics.lux==expected);
        assert(metrics.has_uv_lux && metrics.uv_lux==1.25f);
        assert(metrics.has_white_lux && metrics.white_lux==51 && !metrics.has_ir_lux);
    };
    verify(report.variant.environment_metrics);
    uint8_t bytes[237];size_t n=0;
    assert(mt::EnvironmentQuery::responseMetrics(report.variant.environment_metrics,77,bytes,sizeof(bytes),n));
    meshtastic_Data envelope{};auto input=pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&input,meshtastic_Data_fields,&envelope)&&envelope.request_id==77);
    meshtastic_Telemetry reply{};input=pb_istream_from_buffer(envelope.payload.bytes,envelope.payload.size);
    assert(pb_decode(&input,meshtastic_Telemetry_fields,&reply));verify(reply.variant.environment_metrics);
}
}
bool device_is_ready(const device *){return true;}
int sensor_sample_fetch(const device *dev){return failedFetch[dev->id]?-EIO:0;}
int sensor_channel_get(const device *dev,sensor_channel channel,sensor_value *out) {
    assert(channel==(models[dev->id]==3?SENSOR_CHAN_ALL:SENSOR_CHAN_LIGHT));
    if(failedChannel[dev->id])return -EIO;
    *out={values[dev->id],0};return 0;
}
int main() {
    unsigned cases=0;
    for(unsigned direct:{0,2,3,4,5,6})for(unsigned reversed:{0,1})
       for(unsigned bad=0;bad<4;bad++)for(unsigned failed=0;failed<4;failed++)
       for(unsigned dark=0;dark<4;dark++)for(bool black:{false,true}) {
           check(direct,black,reversed,failed,bad,dark);++cases;
       }
    std::printf("PASS %u mixed optical periodic and phone cases, native %u/%u, bus order, failures, darkness, independent white/UV\n",
                cases,models[0],models[1]);
}
