// SPDX-License-Identifier: GPL-3.0-only
#include "stock/telemetry/SensorCadence.h"
#include "TxAdmission.h"
#include <meshtastic/config.pb.h>
#include <cassert>
#include <initializer_list>

namespace {
uint32_t now, interval, numOnlineNodes, default_telemetry_broadcast_interval_secs;
struct { struct {uint32_t health_update_interval;} telemetry; } moduleConfig;
struct { struct {meshtastic_Config_DeviceConfig_Role role;} device; } config;
enum class TrafficType { TELEMETRY };
struct Default {
    static uint32_t getConfiguredOrDefaultMsScaled(uint32_t,uint32_t,uint32_t,TrafficType){return interval;}
};
struct Throttle {
    static bool isWithinTimespanMs(uint32_t last,uint32_t duration){return now-last<duration;}
};
struct {
    unsigned percent=0;
    bool duty=false;
    bool isTxAllowedChannelUtil(bool polite)const{return percent<(polite?25u:40u);}
    bool isTxAllowedAirUtil()const{return duty;}
} airTimeModel;
auto *airTime=&airTimeModel;
#include "periodic.inc"
}

void testSensorSchedule()
{
    using mt::stock::telemetry::SensorCadence;
    SensorCadence disabled;
    assert(!disabled.beginPoll(1000000));
    for(bool air : {false,true}) for(unsigned percent : {0u,24u,25u,40u,100u})
    for(bool duty : {false,true}) for(auto role : {meshtastic_Config_DeviceConfig_Role_CLIENT,
            meshtastic_Config_DeviceConfig_Role_ROUTER,meshtastic_Config_DeviceConfig_Role_ROUTER_LATE,
            meshtastic_Config_DeviceConfig_Role_TAK_TRACKER,meshtastic_Config_DeviceConfig_Role_CLIENT_BASE}) {
        const uint64_t start=UINT32_MAX-170000u;
        const uint32_t first=air?85000:100000;
        SensorCadence cadence;
        cadence.configure(true,180,start,first);
        assert(!cadence.beginPoll(start+first-1));
        uint32_t last=0;
        config.device.role=role;interval=180000;
        for(unsigned i=0;i!=8;++i){
            const uint64_t tick=start+first+60000ull*i;
            now=uint32_t(tick);
            airTimeModel.percent=percent; airTimeModel.duty=duty;
            assert(cadence.beginPoll(tick));
            bool expected=air?stockAir(last,interval):stockHealth(last);
            const bool actual=cadence.meshDue(tick,interval) && mt::TxAdmission::channel(true,percent,true)==mt::TxAdmission::Decision::Allow && duty;
            assert(expected==actual);
            if(actual && i!=0){last=now;cadence.queued(tick);} // One refused enqueue retries at the next poll.
            cadence.configure(true,180,tick,first); // An unrelated settings revision preserves history.
            assert(!cadence.beginPoll(tick+59999));
        }
        cadence.configure(false,180,start+600000,first);
        assert(!cadence.beginPoll(start+1000000));
    }
}
