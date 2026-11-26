// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "telemetry/CompletionTelemetry.h"
#include "stock/telemetry/SensorModules.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "stock/telemetry/accessory/Pmsa003i.h"
#include "oracle.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
namespace {
std::array<uint8_t,29> hmFrame{};
std::array<uint8_t,32> pmsFrame{};
uint8_t donorAddress=0x40;bool shortRead=false;unsigned pmsWake=0,pmsSleep=0,pmsReads=0;
unsigned hmSelects=0,hmReads=0,donorReads=0,donorSelects=0,donorCursor=0;
uint64_t donorMs=0;bool readError=false;
void fillHm() {
    for (unsigned i=0;i<28;++i) hmFrame[i]=uint8_t(81+i*21);
    hmFrame[28]=0;for(unsigned i=0;i<28;++i)hmFrame[28]+=hmFrame[i];
}
void tick(unsigned count=1) {
    mockKernel::timerTicks=count;
    try {mockKernel::accessoryThread(nullptr,nullptr,nullptr);}catch(mockKernel::NoJob &){}
}
meshtastic_Telemetry decode(const uint8_t *bytes,size_t n,unsigned id) {
    meshtastic_Data d{};auto in=pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&in,meshtastic_Data_fields,&d)&&d.request_id==id);
    meshtastic_Telemetry r{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&r));return r;
}
void fillPms(uint8_t length=28) {
    pmsFrame[0]=0x42;pmsFrame[1]=0x4d;pmsFrame[2]=0;pmsFrame[3]=length;
    for(unsigned i=4;i<30;++i)pmsFrame[i]=uint8_t(i*41);
    uint16_t sum=0;for(unsigned i=0;i<30;++i)sum+=pmsFrame[i];
    pmsFrame[30]=sum>>8;pmsFrame[31]=sum;
}
void compareHm() {
    using mt::telemetry::CompletionTelemetry;
    fillHm();HM330XSensor donor;assert(donor.hm330x.init(&Wire)==HM330XErrorCode::NO_ERROR&&donorSelects==1);
    meshtastic_Telemetry oracle{};assert(donor.getMetrics(&oracle)&&donorReads==1);
    CompletionTelemetry svc;meshtastic_ModuleConfig_TelemetryConfig cfg{};cfg.air_quality_enabled=true;
    tick();assert(!hmReads&&!hmSelects);assert(svc.configSupported(cfg)&&svc.configure(cfg));
    tick(29);assert(hmSelects==1&&!hmReads);tick(2);
    const uint64_t now=k_uptime_get();assert(hmReads&&svc.poll(now)==CompletionTelemetry::Result::Ok);
    CompletionTelemetry::Snapshot snap{};assert(svc.snapshot(now,snap)==CompletionTelemetry::Result::Ok);
    meshtastic_Telemetry announcement{};assert(mt::stock::telemetry::allocReply(meshtastic_Telemetry_air_quality_metrics_tag,snap,10,announcement));
    uint8_t buf[237];size_t n=0;assert(mt::stock::telemetry::encodeReport(announcement,0,buf,sizeof(buf),n));
    auto expected=oracle.variant.air_quality_metrics;
    for(unsigned id:{0,712}){
        if(id)assert(svc.response(meshtastic_Telemetry_air_quality_metrics_tag,id,now,20,buf,sizeof(buf),n)==CompletionTelemetry::Result::Ok);
        auto v=decode(buf,n,id).variant.air_quality_metrics;
        assert(v.has_pm10_standard&&v.has_pm25_standard&&v.has_pm100_standard);
        assert(v.pm10_standard==expected.pm10_standard&&v.pm25_standard==expected.pm25_standard&&v.pm100_standard==expected.pm100_standard);
        assert(!v.has_pm10_environmental&&!v.has_pm25_environmental&&!v.has_pm100_environmental&&!v.has_particles_03um);
    }
    hmFrame[28]^=1;assert(!donor.getMetrics(&oracle));tick();assert(svc.poll(k_uptime_get())==CompletionTelemetry::Result::Unavailable);
    hmFrame[28]^=1;readError=true;tick();readError=false;tick(1);assert(hmSelects==2&&svc.poll(k_uptime_get())==CompletionTelemetry::Result::Unavailable);
    tick(30);assert(svc.poll(k_uptime_get())==CompletionTelemetry::Result::Ok);
    cfg.air_quality_enabled=false;svc.configure(cfg);unsigned before=hmReads;tick(2);assert(hmReads==before);
    puts("PASS native HM330x alias, Seeed protocol/CRC, 30-second warm-up, exact standard mass fields, wire and recovery");
}
#ifdef CONFIG_MT_PMSA003I
void pmsTest() {
    using mt::telemetry::CompletionTelemetry;
    using namespace mt::stock::telemetry::accessory;
    fillPms(27); // Stock accepts a complete checksum-valid sample with a nonstandard length word.
    PMSA003ISensor stock;
    stock.sleep();assert(!stock.isActive()&&stock.canSleep()&&stock.wakeUpTimeMs()==30000);
    meshtastic_Telemetry oracle{};assert(!stock.getMetrics(&oracle));
    assert(stock.wakeUp()==30000&&stock.pendingForReadyMs()==30000);
    donorMs+=29999;assert(stock.pendingForReadyMs()==1);++donorMs;
    assert(!stock.pendingForReadyMs()&&stock.getMetrics(&oracle));
    const auto expected=oracle.variant.air_quality_metrics;
    tick();assert(pmsSleep==1&&!pmsReads&&!pmsWake);
    CompletionTelemetry svc;meshtastic_ModuleConfig_TelemetryConfig cfg{};cfg.air_quality_enabled=true;cfg.air_quality_interval=120;
    assert(svc.configSupported(cfg)&&svc.configure(cfg));uint64_t at=k_uptime_get();
    airPollDeadline(at+85000);
    tick(50);assert(!pmsWake&&!pmsReads);
    tick(5);assert(pmsWake==1&&!pmsReads);
    tick(27);assert(!pmsReads);
    tick(3);assert(pmsReads&&pmsSleep==2);
    auto now=k_uptime_get();assert(svc.poll(now)==CompletionTelemetry::Result::Ok);
    unsigned reads=pmsReads;airPollDeadline(at+145000);tick(20);assert(pmsReads==reads&&pmsWake==1);
    CompletionTelemetry::Snapshot snap{};assert(svc.snapshot(now,snap)==CompletionTelemetry::Result::Ok);
    meshtastic_Telemetry a{};assert(mt::stock::telemetry::allocReply(meshtastic_Telemetry_air_quality_metrics_tag,snap,1,a));
    uint8_t bytes[237];size_t n=0;assert(mt::stock::telemetry::encodeReport(a,0,bytes,sizeof(bytes),n));
    for(unsigned id:{0,34}){
        if(id)assert(svc.response(meshtastic_Telemetry_air_quality_metrics_tag,id,now,2,bytes,sizeof(bytes),n)==CompletionTelemetry::Result::Ok);
        auto pm=decode(bytes,n,id).variant.air_quality_metrics;
#define EXPECT(field) assert(pm.has_##field&&pm.field==expected.field)
        EXPECT(pm10_standard);EXPECT(pm25_standard);EXPECT(pm100_standard);
        EXPECT(pm10_environmental);EXPECT(pm25_environmental);EXPECT(pm100_environmental);
        EXPECT(particles_03um);EXPECT(particles_05um);EXPECT(particles_10um);EXPECT(particles_25um);EXPECT(particles_50um);EXPECT(particles_100um);
#undef EXPECT
    }
    cfg.air_quality_interval=15;svc.configure(cfg);tick(31);auto sleepCount=pmsSleep,initial=pmsReads;tick(3);
    assert(pmsSleep==sleepCount&&pmsReads>initial);
    pmsFrame[0]=1;assert(!stock.getMetrics(&oracle));tick();assert(svc.poll(k_uptime_get())==CompletionTelemetry::Result::Unavailable);
    fillPms();pmsFrame[31]^=1;assert(!stock.getMetrics(&oracle));tick();assert(svc.poll(k_uptime_get())==CompletionTelemetry::Result::Unavailable);
    fillPms();shortRead=true;assert(!stock.getMetrics(&oracle));tick();assert(svc.poll(k_uptime_get())==CompletionTelemetry::Result::Unavailable);
    shortRead=false;tick();assert(svc.poll(k_uptime_get())==CompletionTelemetry::Result::Ok);
    cfg.air_quality_enabled=false;svc.configure(cfg);tick();assert(pmsSleep==sleepCount+1);
    struct Unswitched:Bus{
        uint64_t at=0;uint64_t now()override{return at;}void wait(unsigned)override{}uint32_t utc()override{return 0;}
        int read(uint8_t *out,size_t n)override{assert(n==32);memcpy(out,pmsFrame.data(),n);return 0;}
        int write(const uint8_t *,size_t)override{assert(false);return -EIO;}
        int readReg(uint8_t,uint8_t *,size_t)override{assert(false);return -EIO;}
        int load(const char *,void *,size_t)override{return -ENOENT;}int save(const char *,const void *,size_t)override{return -ENOTSUP;}
    } bus;
    PlantowerPm v;Pmsa003i unswitched(bus,nullptr);assert(!unswitched.measure(v,120,0,false)&&v.counts[5]==expected.particles_100um);
    Pmsa003i longRunning(bus,[](bool){return 0;});
    bus.at=UINT32_MAX-11000u;assert(longRunning.measure(v,15,0,false)==-EAGAIN);
    bus.at+=29999;assert(longRunning.measure(v,15,0,false)==-EAGAIN);
    ++bus.at;assert(!longRunning.measure(v,15,0,false));
    bus.at+=UINT32_C(0x80000001);assert(!longRunning.measure(v,15,0,false));
    puts("PASS PMSA003I bare frame and 12 donor fields, checksum/short/fault rejection, active-high SET warm-up and sleep against periodic owner, no-pin immediate sample and both wire paths");
}
#endif
}
TwoWire Wire;
void TwoWire::beginTransmission(uint8_t a){assert(a==0x40);}
size_t TwoWire::write(uint8_t cmd){assert(cmd==0x88);++donorSelects;return 1;}
uint8_t TwoWire::endTransmission(bool stop){assert(stop);return 0;}
uint8_t TwoWire::requestFrom(uint8_t a,size_t n){assert((a==0x40&&n==29)||(a==0x12&&n==32));donorAddress=a;++donorReads;donorCursor=0;return shortRead?n-1:n;}
int TwoWire::available(){return int((donorAddress==0x40?hmFrame.size():pmsFrame.size())-donorCursor-(shortRead?1:0));}
int TwoWire::read(){assert(available());return donorAddress==0x40?hmFrame[donorCursor++]:pmsFrame[donorCursor++];}
void digitalWrite(unsigned pin,unsigned state){assert(pin==7&&state<=1);}
bool gpio_is_ready_dt(const gpio_dt_spec *){return true;}
int gpio_pin_configure_dt(const gpio_dt_spec *,int f){if(f==GPIO_OUTPUT_ACTIVE)++pmsWake;else {assert(f==GPIO_OUTPUT_INACTIVE);++pmsSleep;}return 0;}
void delay(unsigned ms){donorMs+=ms;}unsigned long millis(){return donorMs;}
bool device_is_ready(const device *){return false;}
int sensor_sample_fetch(const device *){assert(false);return -ENODEV;}
int sensor_channel_get(const device *,sensor_channel,sensor_value *){assert(false);return -ENODEV;}
bool i2c_is_ready_dt(const i2c_dt_spec *){return true;}
int i2c_write_dt(const i2c_dt_spec *d,const uint8_t *p,size_t n){
    assert(d->address==0x40&&n==1&&*p==0x88);++hmSelects;return 0;
}
int i2c_read_dt(const i2c_dt_spec *d,uint8_t *p,size_t n){
    if(d->address==0x12){assert(n==32);++pmsReads;if(shortRead)return -EIO;memcpy(p,pmsFrame.data(),n);return 0;}
    assert(d->address==0x40&&n==29);++hmReads;if(readError)return -EIO;memcpy(p,hmFrame.data(),n);return 0;
}
int i2c_write_read_dt(const i2c_dt_spec *,const uint8_t *,size_t,uint8_t *,size_t){assert(false);return -EIO;}
ssize_t settings_get_val_len(const char *){return -ENOENT;}
ssize_t settings_load_one(const char *,void *,size_t){return -ENOENT;}
int settings_save_one(const char *,const void *,size_t){return -ENOTSUP;}
int main(){
#ifdef CONFIG_MT_PMSA003I
    pmsTest();
#else
    compareHm();
#endif
}
