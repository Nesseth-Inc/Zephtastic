// SPDX-License-Identifier: GPL-3.0-only
#include "RemoteTelemetryQuery.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

struct Reference {
#include "telemetry_numeric_reference.inc"
};
static unsigned fields,checks;
static void boundary(meshtastic_Telemetry &t,bool expected){
    assert(Reference::numeric(t)==expected);
    uint8_t bytes[233];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&out,meshtastic_Telemetry_fields,&t));
    meshtastic_Telemetry decoded{};
    assert(mt::RemoteTelemetryQuery::decodeCanonical(bytes,out.bytes_written,t.which_variant,decoded)==expected);
    assert(!!decoded.which_variant==expected);++checks;
}
static void verify(meshtastic_Telemetry &t,float &value,bool *present){
    ++fields;for(uint32_t bits:{0u,0x80000000u,1u,0x80000001u,0x7f7fffffu,0xff7fffffu,
                                0x7f800000u,0xff800000u,0x7fc00021u,0xffc00021u,0x7f800001u,0xff800001u}){
        memcpy(&value,&bits,4);
        if(present){*present=false;boundary(t,true);*present=true;}
        boundary(t,(bits&0x7f800000u)!=0x7f800000u);
    }
    if(present)*present=false;
    value=0;
}
int main(){
    meshtastic_Telemetry t{};
#define TEST_OPTIONAL(r,m) verify(t,r.m,&r.has_##m);
#define TEST_SINGULAR(r,m) verify(t,r.m,nullptr);
#define TEST_FLOAT(r,rule,m) TEST_##rule(r,m)
#define TEST_UINT32(r,rule,m)
#define TEST_UINT64(r,rule,m)
#define TEST_INT32(r,rule,m)
#define TEST_STRING(r,rule,m)
#define TEST(r,storage,rule,type,name,tag) TEST_##type(r,rule,name)
#define METRICS(member,type) t={};t.time=0x7fc00111;t.which_variant=meshtastic_Telemetry_##member##_tag; \
    meshtastic_##type##_FIELDLIST(TEST,t.variant.member) boundary(t,true);
    METRICS(device_metrics,DeviceMetrics)
    METRICS(environment_metrics,EnvironmentMetrics)
    METRICS(air_quality_metrics,AirQualityMetrics)
    METRICS(power_metrics,PowerMetrics)
    METRICS(local_stats,LocalStats)
    METRICS(health_metrics,HealthMetrics)
    METRICS(host_metrics,HostMetrics)
    METRICS(traffic_management_stats,TrafficManagementStats)
    METRICS(soil_water_metrics,SoilWaterMetrics)
    assert(fields==83);
    // Integer payloads can have the same bits as a NaN. Optional presence
    // belongs to each individual float within the selected union member.
    t={};t.which_variant=meshtastic_Telemetry_device_metrics_tag;
    auto &d=t.variant.device_metrics;d.battery_level=0x7fc00111;d.has_battery_level=true;
    d.has_channel_utilization=true;d.channel_utilization=-1;boundary(t,true);
    uint32_t nan=0x7fc00111;memcpy(&d.air_util_tx,&nan,4);boundary(t,true);
    d.has_air_util_tx=true;boundary(t,false);
    printf("PASS all %u generated floating fields and %u prior-policy/real canonical codec comparisons\n",fields,checks);
}
