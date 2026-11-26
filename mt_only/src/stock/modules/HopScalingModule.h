// SPDX-License-Identifier: GPL-3.0-only
// Port of Meshtastic 8e6a88d, src/modules/HopScalingModule.{h,cpp}. ZT uses
// the live RF histogram and the stock non-filesystem cold-start behavior.
#pragma once
#include "meshtastic/config.pb.h"
#include "meshtastic/portnums.pb.h"
#include <stdint.h>

namespace mt::stock {
class HopScalingModule {
    struct Record { uint32_t hash:16, hops:3, seen:13; };
    static_assert(sizeof(Record)==4);
    static constexpr unsigned Capacity=128, MaximumHop=7, RollsPerHour=12;
    Record records_[Capacity]{};
    uint16_t seed_=0;
    uint8_t count_=0, sampling_=1, filtering_=1, hold_=0;
    uint8_t suggested_=MaximumHop, required_=MaximumHop, confirm_=0, runs_=0;
    bool congested_=false, started_=false, seeded_=false;
    uint32_t due_=30000;

    static bool passes(uint16_t hash,unsigned denominator) { return !(hash & (denominator-1)); }

    void trimIfNeeded() {
        unsigned kept=0;
        for(unsigned i=0;i<count_;++i)
            if(records_[i].seen) records_[kept++]=records_[i];
        count_=kept;
        if(count_*100u/Capacity<80 || sampling_>=128) return;
        sampling_*=2;
        if(filtering_<sampling_) filtering_=sampling_;
        hold_=13;
        kept=0;
        for(unsigned i=0;i<count_;++i)
            if(passes(records_[i].hash,sampling_)) records_[kept++]=records_[i];
        count_=kept;
    }

    void rollHour(unsigned utilization) {
        uint16_t perHop[MaximumHop+1]{};
        unsigned total=0;
        for(unsigned i=0;i<count_;++i) {
            auto &r=records_[i];
            if(passes(r.hash,filtering_) && r.seen) {
                ++perHop[r.hops];
                ++total;
            }
            r.seen=(r.seen<<1)&0x1fffu;
        }
        // Find the first hop reaching 40 estimated nodes. Allow one more hop
        // when the expected population stays under the channel's polite limit.
        suggested_=MaximumHop;
        const unsigned polite=utilization>=25 ? 1 : utilization>=20 ? 2 : 4;
        unsigned cumulative=0;
        for(unsigned h=0;h<=MaximumHop;++h) {
            cumulative+=perHop[h]*filtering_;
            if(cumulative<40) continue;
            suggested_=h;
            if(h<MaximumHop && (cumulative+perHop[h+1]*filtering_)*4<=40*4+(80-40)*polite)
                ++suggested_;
            break;
        }
        if(total*100u<Capacity*20 && sampling_>1) sampling_/=2;
        if(filtering_>sampling_) {
            if(hold_) --hold_;
            if(!hold_) filtering_/=2;
        }
    }

public:
    void seed(uint16_t value) {
        if(!seeded_) { seed_=value; seeded_=true; }
    }
    void samplePacketForHistogram(uint32_t source,unsigned hops) {
        const uint16_t hash=uint16_t((source*2654435761u)>>16)^seed_;
        if(!passes(hash,sampling_))return;
        if(hops>MaximumHop)hops=MaximumHop;
        for(unsigned i=0;i<count_;++i) {
            if(records_[i].hash!=hash)continue;
            records_[i].hops=hops;
            records_[i].seen|=1;
            return;
        }
        if(count_*100u/Capacity>=80)trimIfNeeded();
        if(count_<Capacity)records_[count_++]={hash,hops,1};
    }

    bool due(uint32_t now) const { return int32_t(now-due_)>=0; }
    void runOnce(uint32_t now,float smoothed,unsigned role) {
        due_=now+300000u;
        const unsigned util=unsigned(smoothed>=254.5f ? 255 : smoothed+0.5f);
        const bool flip=congested_ ? util<=12 : util>=20;
        confirm_=flip ? confirm_+1 : 0;
        if(confirm_>=3) {congested_=!congested_;confirm_=0;}
        if(!started_) {started_=true;return;}
        if(++runs_<RollsPerHour)return;
        runs_=0;
        rollHour(util);
        if(!congested_) {
            if(required_<MaximumHop)++required_;
            return;
        }
        uint8_t floor=0;
        switch(role) {
        case meshtastic_Config_DeviceConfig_Role_ROUTER:
        case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
        case meshtastic_Config_DeviceConfig_Role_CLIENT_BASE:floor=3;break;
        case meshtastic_Config_DeviceConfig_Role_TRACKER:
        case meshtastic_Config_DeviceConfig_Role_TAK_TRACKER:floor=2;break;
        case meshtastic_Config_DeviceConfig_Role_SENSOR:floor=1;break;
        }
        const auto hop=count_ ? suggested_ : uint8_t(MaximumHop);
        required_=hop<floor ? floor : hop;
    }

    // Router::send: limit the four routine local broadcasts. Direct traffic,
    // relay frames, text and emergency messages retain their requested hop limit.
    unsigned limit(unsigned hop,uint32_t to,unsigned port) const {
        if(to==UINT32_MAX && (port==meshtastic_PortNum_POSITION_APP||port==meshtastic_PortNum_TELEMETRY_APP||
           port==meshtastic_PortNum_NODEINFO_APP||port==meshtastic_PortNum_NEIGHBORINFO_APP))
            return hop<required_?hop:required_;
        return hop;
    }
};
}
