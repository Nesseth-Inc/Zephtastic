// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/ZephyrEnvironmentChannels.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "stock/telemetry/accessory/TslLight.h"
#include "oracle.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace mt::stock::telemetry::accessory;
namespace {
#ifdef TEST_TSL2561
constexpr bool newer=false;
TSL2561Sensor oracle;
#else
constexpr bool newer=true;
TSL2591Sensor oracle;
#endif
struct Chip {
    uint16_t full=1000,ir=305;
    uint8_t id=0x50,config=0,current=0;
    int badRead=-1, badWrite=-1;bool on=false;
    uint64_t at=0,ms=0;std::vector<unsigned> sampled;
    bool put(uint8_t reg,uint8_t val) {
        if(reg==badWrite)return false;
        if(reg==(newer?0xa0:0x80)){assert(val==0||val==(newer?0x93:3));on=val;if(on)at=ms;}
        else {assert(reg==(newer?0xa1:0x81));assert(on);config=val;}
        return true;
    }
    bool get(unsigned reg,uint8_t *out,size_t n) {
        if(int(reg)==badRead)return false;
        if(n==1){assert(reg==(newer?0xb2:0x8a)||(!newer&&reg==0x0a));out[0]=id;return true;}
        assert(n==2 && on && config==(newer?0:1) && ms-at>=120);
        const bool ch0=reg==(newer?0xb4:0xac);
        assert(ch0||reg==(newer?0xb6:0xae));
        sampled.push_back(reg);
        uint16_t v=ch0?full:ir;out[0]=v;out[1]=v>>8;return true;
    }
} chip, donorChip;
std::array<uint8_t,4> wireBuf{};
unsigned txN=0,rxN=0,rxPos=0;
void work(unsigned n=1) {
    mockKernel::timerTicks=n;
    try {mockKernel::accessoryThread(nullptr,nullptr,nullptr);} catch (const mockKernel::NoJob &) {}
}
struct Port : Bus {
    Chip sensor;
    int write(const uint8_t *p,size_t n) override {
        if(n==1){assert(!newer);sensor.current=p[0];return 0;}
        assert(n==2);return sensor.put(p[0],p[1])?0:-EIO;
    }
    int read(uint8_t *p,size_t n)override {assert(!newer);return sensor.get(sensor.current,p,n)?0:-EIO;}
    int readReg(uint8_t r,uint8_t *p,size_t n)override{assert(newer);return sensor.get(r,p,n)?0:-EIO;}
    void wait(unsigned ms)override{assert(ms==120);sensor.ms+=ms;}
    uint64_t now()override{return sensor.ms;}
    uint32_t utc()override{return 0;}
    int load(const char*,void*,size_t)override{return -ENOENT;}
    int save(const char*,const void*,size_t)override{return -ENOTSUP;}
};
void compare() {
    Port port;
    TslLight<newer> light(port);
    assert(oracle.initDevice(&Wire,nullptr));
    std::vector<uint16_t> values{0,1,2,37,100,125,250,375,500,610,800,1000,1300,4095,15000,36999,37000,37001,65534,65535};
    for (uint16_t full:values) for (uint16_t ir:values) {
        port.sensor.full=donorChip.full=full;port.sensor.ir=donorChip.ir=ir;
        float lux=0; bool measured=light.sample(lux);
        meshtastic_Telemetry expected{};assert(oracle.getMetrics(&expected));
        float value=expected.variant.environment_metrics.lux;
        bool valid=std::isfinite(value)&&value>=0;
        assert(valid==measured);if(valid)assert(value==lux);
        assert(!port.sensor.on && !donorChip.on && port.sensor.sampled==donorChip.sampled);
        port.sensor.sampled.clear();donorChip.sampled.clear();
    }
    puts("PASS 400 pinned stock and Adafruit lux comparisons for the selected model: gain, integration, channel order, IR ratio, clipping and darkness");
}
void integration() {
    work();assert(chip.sampled.empty());enableEnvironment(true);work();
    auto current=environment(k_uptime_get());assert(current.hasTslLux);
    current.hasLight=true;current.lux=100;current.uvIndex=2;
    zt::telemetry::EnvironmentReadings readings;readings.accessory=current;
    meshtastic_Telemetry report{};assert(mt::stock::telemetry::environmentReport(report,100,readings));
    auto &m=report.variant.environment_metrics;assert(m.has_lux&&m.lux==current.tslLux&&m.has_uv_lux&&m.uv_lux==2&&!m.has_ir_lux);
    uint8_t bytes[237];size_t n;
    assert(mt::EnvironmentQuery::responseMetrics(m,3,bytes,sizeof(bytes),n));
    meshtastic_Data data{};auto input=pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&input,meshtastic_Data_fields,&data)&&data.request_id==3);
    meshtastic_Telemetry reply{};input=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&input,meshtastic_Telemetry_fields,&reply)&&reply.variant.environment_metrics.lux==m.lux&&!reply.variant.environment_metrics.has_ir_lux);
    chip.badRead=newer?0xb6:0xae;work();assert(!environment(k_uptime_get()).hasTslLux&&!chip.on);
    chip.badRead=-1;chip.id=0xff;work();assert(!environment(k_uptime_get()).hasTslLux&&!chip.on);
    chip.id=0x50;work();assert(environment(k_uptime_get()).hasTslLux);
    chip.badWrite=newer?0xa0:0x80;work();assert(!environment(k_uptime_get()).hasTslLux);
    chip.badWrite=-1;work();assert(environment(k_uptime_get()).hasTslLux);
    assert(!environment(k_uptime_get()+90001).hasTslLux);
    enableEnvironment(false);work();assert(!chip.on&&!environment(k_uptime_get()).hasTslLux);
    enableEnvironment(true);work();assert(environment(k_uptime_get()).hasTslLux);
    // The existing native alias also exposes paired lux, never raw relative strengths.
    zt::telemetry::EnvironmentReadings generic;
    mt::stock::telemetry::readEnvironmentChannels(DEVICE_DT_GET(zt_environment),generic);
    auto native=zt::telemetry::toMeshtasticEnvironmentMetrics(generic);
    assert(native.has_lux&&native.lux==76.25f&&!native.has_ir_lux);
    puts("PASS stock worker and solicited/periodic fields, sole lux from native TSL2591, error cleanup, expiry and re-enable");
}
}
TwoWire Wire;
unsigned long millis(){return donorChip.ms;}
void delay(unsigned long n){assert(n==120);donorChip.ms+=n;}
void TwoWire::beginTransmission(uint8_t a){assert(!newer&&a==0x29);txN=0;}
void TwoWire::write(uint8_t n){assert(txN<wireBuf.size());wireBuf[txN++]=n;}
void TwoWire::endTransmission(){assert(txN==1||txN==2);if(txN==1)donorChip.current=wireBuf[0];else assert(donorChip.put(wireBuf[0],wireBuf[1]));}
void TwoWire::requestFrom(uint8_t a,unsigned n){assert(a==0x29&&n<=wireBuf.size());rxN=n;rxPos=0;assert(donorChip.get(donorChip.current,wireBuf.data(),n));}
uint8_t TwoWire::read(){assert(rxPos<rxN);return wireBuf[rxPos++];}
bool Adafruit_I2CDevice::write_then_read(const uint8_t *p,size_t n,uint8_t *r,size_t len){assert(newer&&n==1);return donorChip.get(*p,r,len);}
bool Adafruit_I2CDevice::write(const uint8_t *p,size_t n){assert(newer&&n==2);return donorChip.put(p[0],p[1]);}
bool device_is_ready(const device *d){return d==DEVICE_DT_GET(zt_environment);}
int sensor_sample_fetch(const device *d){assert(device_is_ready(d));return 0;}
int sensor_channel_get(const device *,sensor_channel chan,sensor_value *value){assert(chan==SENSOR_CHAN_ALL);*value={76,250000};return 0;}
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n){
    assert(s->address==0x29);chip.ms=k_uptime_get();
    if(n==1){assert(!newer);chip.current=*p;return 0;}
    assert(n==2);return chip.put(p[0],p[1])?0:-EIO;
}
int i2c_write_read_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n,uint8_t *out,size_t len){
    assert(newer&&s->address==0x29&&n==1);chip.ms=k_uptime_get();return chip.get(*p,out,len)?0:-EIO;
}
int i2c_read_dt(const i2c_dt_spec *s,uint8_t *out,size_t n){assert(!newer&&s->address==0x29);chip.ms=k_uptime_get();return chip.get(chip.current,out,n)?0:-EIO;}
ssize_t settings_get_val_len(const char*){return -ENOENT;}
ssize_t settings_load_one(const char*,void*,size_t){return -ENOENT;}
int settings_save_one(const char*,const void*,size_t){return -ENOTSUP;}
int main(){compare();integration();}
