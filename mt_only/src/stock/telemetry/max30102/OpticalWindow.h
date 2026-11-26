// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <meshtastic/telemetry.pb.h>
#include <stdint.h>
namespace mt::stock::telemetry::max30102 {
void calculate(uint32_t (&ir)[100],uint32_t (&red)[100],meshtastic_HealthMetrics &out);
// Single main-thread owner. Each pair comes from a FIFO sample on a declared
// MAX30102 configured for 100 Hz with 4-sample averaging (25 effective Hz).
class OpticalWindow {
    uint32_t ir_[100]{},red_[100]{};
    unsigned next_=0;
    meshtastic_HealthMetrics latest_{};
    uint64_t sampledAt_=0;
public:
    void reset(){next_=0;latest_={};sampledAt_=0;}
    bool add(uint32_t ir,uint32_t red,uint64_t now){
        if(ir>0x3ffff||red>0x3ffff){reset();return false;}
        ir_[next_]=ir;red_[next_]=red;
        if(++next_!=100)return false;
        next_=0;calculate(ir_,red_,latest_);sampledAt_=now;return true;
    }
    bool snapshot(uint64_t now,meshtastic_HealthMetrics &out)const{
        if(now<sampledAt_||now-sampledAt_>10000||(!latest_.has_spO2&&!latest_.has_heart_bpm))return false;
        if(latest_.has_spO2){out.has_spO2=true;out.spO2=latest_.spO2;}
        if(latest_.has_heart_bpm){out.has_heart_bpm=true;out.heart_bpm=latest_.heart_bpm;}
        return true;
    }
};
bool available();
void poll(bool enabled,uint64_t now);
bool sample(uint64_t now,meshtastic_HealthMetrics &out);
}
