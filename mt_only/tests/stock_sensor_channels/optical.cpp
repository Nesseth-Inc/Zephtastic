// SPDX-License-Identifier: GPL-3.0-only
#include "stock/telemetry/max30102/OpticalWindow.h"
#include "telemetry/CompletionTelemetry.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#ifdef MT_MAXIM_ORACLE
void maxim_heart_rate_and_oxygen_saturation(uint32_t*,int32_t,uint32_t*,int32_t*,int8_t*,int32_t*,int8_t*);
#endif
namespace optical=mt::stock::telemetry::max30102;
namespace {
uint8_t writePos=0,readPos=0,overflow=0;unsigned fetches=0;
bool ready=true;
struct Pair{uint32_t red,ir;};
Pair fifo[32]{},current{};
}
bool i2c_is_ready_dt(const i2c_dt_spec *bus){return ready&&bus->address==0x57;}
int i2c_burst_read_dt(const i2c_dt_spec*,uint8_t reg,uint8_t *out,size_t n){
    assert(reg==4&&n==3);out[0]=writePos;out[1]=overflow;out[2]=readPos;return 0;
}
int i2c_reg_write_byte_dt(const i2c_dt_spec*,uint8_t reg,uint8_t value){
    if(reg==5)overflow=value;else{assert(reg==6);readPos=value;}return 0;
}
int opticalFetch(){assert(readPos!=writePos);current=fifo[readPos];readPos=(readPos+1)%32;++fetches;return 0;}
int opticalGet(sensor_channel channel,sensor_value *v){
    if(channel!=SENSOR_CHAN_RED&&channel!=SENSOR_CHAN_IR)return -1;
    v->val1=int32_t(channel==SENSOR_CHAN_RED?current.red:current.ir);v->val2=0;return 0;
}
static void add(uint32_t ir,uint32_t red){
    fifo[writePos]={red,ir};writePos=(writePos+1)%32;
}
void opticalCases(){
    uint32_t ir[100],red[100];
    auto signal=[&](unsigned i){
        double angle=(i+.21)*(2*3.141592653589793/20);
        ir[i]=uint32_t(32000+600*std::sin(angle));
        red[i]=uint32_t(34500+510*std::sin(angle+.04));
    };
    for(unsigned i=0;i<100;++i)signal(i);
    meshtastic_HealthMetrics expected{};optical::calculate(ir,red,expected);
    assert(expected.has_heart_bpm&&expected.heart_bpm==75&&expected.has_spO2&&expected.spO2==91);
#ifdef MT_MAXIM_ORACLE
    int32_t bpm,oxygen;int8_t validBpm,validOxygen;
    maxim_heart_rate_and_oxygen_saturation(ir,100,red,&oxygen,&validOxygen,&bpm,&validBpm);
    assert(validBpm&&validOxygen&&bpm==expected.heart_bpm&&oxygen==expected.spO2);
#endif
    meshtastic_HealthMetrics actual{};
    ready=false;assert(!optical::available());optical::poll(true,0);assert(!optical::sample(0,actual));
    ready=true;optical::poll(true,1);assert(!optical::sample(1,actual));
    uint64_t now=1;
    for(unsigned i=0;i<50;++i){add(ir[i],red[i]);now+=40;optical::poll(true,now);}
    overflow=1;now+=40;optical::poll(true,now);assert(!overflow&&!optical::sample(now,actual));
    const unsigned initial=fetches;
    for(unsigned i=0;i<100;++i){
        add(ir[i],red[i]);now+=40;optical::poll(true,now);
        assert(optical::sample(now,actual)==(i==99));
    }
    assert(fetches==initial+100&&actual.heart_bpm==expected.heart_bpm&&actual.spO2==expected.spO2);
    now+=40;optical::poll(true,now);assert(fetches==initial+100);
    mt::telemetry::CompletionTelemetry module;meshtastic_ModuleConfig_TelemetryConfig cfg{};cfg.health_measurement_enabled=true;
    assert(module.configSupported(cfg)&&module.configure(cfg));
    assert(module.poll(now)==mt::telemetry::CompletionTelemetry::Result::Ok);
    mt::telemetry::CompletionTelemetry::Snapshot snapshot{};
    assert(module.snapshot(now,snapshot)==mt::telemetry::CompletionTelemetry::Result::Ok);
    assert(snapshot.health.has_heart_bpm&&snapshot.health.spO2==expected.spO2);
    assert(!optical::sample(now+10001,actual));
    optical::poll(false,now);assert(!optical::sample(now,actual));
    optical::OpticalWindow window;
    for(unsigned i=0;i<100;++i)window.add(123456,123456,now+i);
    assert(!window.snapshot(now+100,actual));
    for(unsigned i=0;i<100;++i)window.add(i%19<9?0x3ffff:0, i%13<5?0x3ffff:0,now+i);
    assert(!window.add(0x40000,100,now+101)&&!window.snapshot(now+101,actual));
    printf("PASS MAX30102 100-sample FIFO and stock signal %u BPM, %u%%, overflow reset, no-contact, absent, stale and 18-bit input bounds\n",
           expected.heart_bpm,expected.spO2);
}
