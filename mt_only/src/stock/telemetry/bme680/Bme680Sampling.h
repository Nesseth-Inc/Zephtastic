// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "BME680IaqEstimator.h"
#include <math.h>
#include <stdint.h>
#include <meshtastic/telemetry.pb.h>

namespace mt::stock::telemetry::bme680 {
struct Reading {float temperature,humidity,pressureHpa,gasOhms;};

// Sampling, freshness and checkpoint cadence from stock BME680Sensor at 8e6a88d.
// The platform owns measurement and the verified settings transaction.
class Sampling {
    BME680IaqEstimator estimator_{};
    Reading last_{};
    bool sampled_=false,iaqValid_=false;
    uint16_t iaq_=0;
    uint64_t sampledAt_=0,iaqAt_=0,savedAt_=0;
    uint32_t savedCount_=UINT32_MAX,savedWarmup_=UINT32_MAX,savedUtc_=0;
public:
    static constexpr uint32_t Interval=60000,Fresh=120000,Carry=600000,Checkpoint=21600000;
    void invalidate(){sampled_=false;iaqValid_=false;}
    void accept(const Reading &r,uint64_t now){
        last_=r;sampled_=true;sampledAt_=now;
        if(estimator_.update(r.gasOhms,r.humidity,&iaq_)){iaqAt_=now;iaqValid_=true;}
        else if((isfinite(r.gasOhms)&&r.gasOhms>0)||now-iaqAt_>=Carry)iaqValid_=false;
    }
    bool metrics(uint64_t now,meshtastic_EnvironmentMetrics &out)const{
        if(!sampled_||now<sampledAt_||now-sampledAt_>=Fresh)return false;
        out.has_temperature=out.has_relative_humidity=out.has_barometric_pressure=true;
        out.temperature=last_.temperature;out.relative_humidity=last_.humidity;out.barometric_pressure=last_.pressureHpa;
        if(isfinite(last_.gasOhms)&&last_.gasOhms>0){out.has_gas_resistance=true;out.gas_resistance=last_.gasOhms/1000.0f;}
        if(iaqValid_){out.has_iaq=true;out.iaq=iaq_;}
        return true;
    }
    bool checkpointDue(uint64_t now,uint32_t utc)const{
        if(!sampled_)return false;
        if(!estimator_.ready())return estimator_.samplesFed()!=savedCount_||estimator_.warmupLeft()!=savedWarmup_;
        if(utc&&savedUtc_)return utc<savedUtc_||utc-savedUtc_>=Checkpoint/1000;
        return estimator_.samplesFed()-savedCount_>=Checkpoint/Interval||now-savedAt_>=Checkpoint;
    }
    void serialize(BME680IaqState &s,uint32_t utc)const{estimator_.serialize(&s,utc);}
    void saved(uint64_t now,uint32_t utc){savedCount_=estimator_.samplesFed();savedWarmup_=estimator_.warmupLeft();savedUtc_=utc;savedAt_=now;}
    bool restore(const BME680IaqState &s,uint64_t now,uint32_t utc){
        if(!estimator_.restore(s,utc))return false;
        saved(now,s.savedAtSecs);return true;
    }
};
}
