// SPDX-License-Identifier: GPL-3.0-only
#include "stock/telemetry/bme680/Bme680Sampling.h"
#include "environment/EnvironmentMetricsAdapter.h"
#include <cassert>
#include <cstring>
using namespace mt::stock::telemetry::bme680;
int main(){
    Sampling s;constexpr Reading clean{0,40,1000,400000};uint64_t now=0;uint32_t utc=1800000000;
    meshtastic_EnvironmentMetrics m{};assert(!s.metrics(now,m));
    BME680IaqState state{};unsigned saves=0;
    for(unsigned i=0;i<100;++i){
        s.accept(clean,now);m={};assert(s.metrics(now,m)&&m.has_temperature&&m.temperature==0&&m.gas_resistance==400);
        if(s.checkpointDue(now,utc)){s.serialize(state,utc);s.saved(now,utc);++saves;}
        if(m.has_iaq)break;
        Sampling reboot;assert(reboot.restore(state,0,utc));s=reboot;now+=s.Interval;utc+=60;
    }
    assert(m.has_iaq&&m.iaq==0&&saves>10);
    zt::telemetry::EnvironmentReadings live{};live.bmeGas=m.gas_resistance;live.bmeIaq=m.iaq;
    auto out=zt::telemetry::toMeshtasticEnvironmentMetrics(live);
    assert(zt::telemetry::hasEnvironmentMetrics(out)&&out.has_iaq&&out.iaq==0&&out.gas_resistance==400);
    assert(s.metrics(now+s.Fresh-1,m)&&!s.metrics(now+s.Fresh,m));
    Reading unstable=clean;unstable.gasOhms=0;
    s.accept(unstable,now+s.Carry-1);m={};assert(s.metrics(now+s.Carry-1,m)&&m.has_iaq&&!m.has_gas_resistance);
    s.accept(unstable,now+s.Carry);m={};assert(s.metrics(now+s.Carry,m)&&!m.has_iaq&&!m.has_gas_resistance);
    s.invalidate();assert(!s.metrics(now+s.Carry,m));
    s.accept(clean,now+s.Carry+1);s.serialize(state,utc);s.saved(now+s.Carry+1,utc);
    assert(!s.checkpointDue(now+s.Carry+1,utc+21599)&&s.checkpointDue(now+s.Carry+1,utc+21600));
}
