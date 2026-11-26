// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "stock/telemetry/accessory/Opt3001.h"
#include "opt-oracle.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace mt::stock::telemetry::accessory;
namespace {
uint16_t sampleWord=0, part=0x3001, maker=0x5449, config=0, flags=0x80;
int failSelect=-1, failRead=-1, failWrite=-1;
bool corrupt=false;
unsigned reads=0, writes=0, selected=0, wireBytes=0;
std::vector<uint8_t> donorTx;
uint16_t donorConfig=0;
constexpr i2c_dt_spec dev{0x45};
void tick(unsigned n=1) {
    mockKernel::timerTicks=n;
    try { mockKernel::accessoryThread(nullptr,nullptr,nullptr); } catch(const mockKernel::NoJob &) {}
}
struct TimeBus : Bus {
    uint64_t ms=0;
    int write(const uint8_t *p,size_t n) override {return i2c_write_dt(&dev,p,n);}
    int read(uint8_t *p,size_t n) override {return i2c_read_dt(&dev,p,n);}
    int readReg(uint8_t,uint8_t*,size_t) override {assert(false);return -EIO;}
    void wait(unsigned) override {assert(false);}
    uint64_t now() override {return ms;}
    uint32_t utc() override {return 0;}
    int load(const char*,void*,size_t) override {return -ENOTSUP;}
    int save(const char*,const void*,size_t) override {return -ENOTSUP;}
};
void timing() {
    TimeBus bus; Opt3001 opt(bus); float lux=-1;
    assert(!opt.sample(lux)&&config==0xc610);
    unsigned before=reads;
    bus.ms=99;assert(!opt.sample(lux)&&reads==before);
    bus.ms=100;flags=0;assert(!opt.sample(lux)&&reads==before+1);
    flags=0x180;assert(!opt.sample(lux)&&lux==-1);
    flags=0x80;assert(opt.sample(lux)&&lux==0);
    failWrite=0xc010;opt.stop();before=reads;
    assert(!opt.sample(lux)&&reads==before);
    failWrite=-1;assert(!opt.sample(lux)&&config==0xc610&&!opt.sample(lux));
    opt.stop();assert(config==0xc010);
}
float report(const Environment &e) {
    zt::telemetry::EnvironmentReadings r;r.accessory=e;
    meshtastic_Telemetry t{};assert(mt::stock::telemetry::environmentReport(t,4,r));
    const auto &m=t.variant.environment_metrics;
    assert(m.has_lux);
    uint8_t bytes[237];size_t n=0;assert(mt::EnvironmentQuery::responseMetrics(m,7,bytes,sizeof(bytes),n));
    meshtastic_Data data{};auto in=pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&in,meshtastic_Data_fields,&data)&&data.request_id==7);
    meshtastic_Telemetry reply{};in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&reply));
    const auto &phone=reply.variant.environment_metrics;
    assert(phone.has_lux&&phone.lux==m.lux&&phone.has_uv_lux==m.has_uv_lux&&!phone.has_white_lux);
    return m.lux;
}
void worker() {
    unsigned before=writes;tick();assert(writes==before);
    enableEnvironment(true);maker=0;tick(2);assert(writes==before&&!environment(k_uptime_get()).hasOptLux);
    maker=0x5449;part=0;tick();assert(writes==before);
    part=0x3001;failSelect=0x7f;tick();assert(writes==before);
    failSelect=-1;failRead=0x7e;tick();assert(writes==before);
    failRead=-1;failWrite=0xc610;tick();assert(config==0xc010&&!environment(k_uptime_get()).hasOptLux);
    failWrite=-1;tick();assert(config==0xc610&&!environment(k_uptime_get()).hasOptLux);
    OPT3001Sensor oracle; ScanI2C::FoundDevice device;assert(oracle.initDevice(&Wire,&device));
    // The pinned source assigns just the range, CT, mode and latch bitfields.
    assert((donorConfig&0xfe10)==0xc610);
    before=writes;
    for(uint16_t word:{0,1,4095,4096,0x982a,0xbfff,0xffff}) {
        sampleWord=word;tick();meshtastic_Telemetry donor{};assert(oracle.getMetrics(&donor));
        auto e=environment(k_uptime_get());
        assert(e.hasOptLux&&e.optLux==donor.variant.environment_metrics.lux&&report(e)==e.optLux);
        e.hasLight=true;e.lux=30;e.uvIndex=3;e.hasTslLux=true;e.tslLux=22;
        assert(report(e)==e.optLux);e.hasTsl2561Lux=true;assert(report(e)==22);
        e.hasBhLux=true;e.bhLux=33;assert(report(e)==33);
    }
    assert(writes==before);
    failRead=0;tick();assert(!environment(k_uptime_get()).hasOptLux&&config==0xc010);
    failRead=-1;corrupt=true;tick(2);assert(!environment(k_uptime_get()).hasOptLux&&config==0xc010);
    corrupt=false;tick(2);assert(environment(k_uptime_get()).hasOptLux);
    enableEnvironment(false);assert(!environment(k_uptime_get()).hasOptLux);tick();assert(config==0xc010);
    enableEnvironment(true);tick(2);assert(environment(k_uptime_get()).hasOptLux);
    assert(!environment(k_uptime_get()+90001).hasOptLux);
}
}
TwoWire Wire;
void delay(unsigned long) {assert(false);}
void TwoWire::beginTransmission(uint8_t a){assert(a==0x45);donorTx.clear();}
void TwoWire::write(uint8_t c){donorTx.push_back(c);}
uint8_t TwoWire::endTransmission(bool stop){
    assert(stop);
    if(donorTx.size()==3){assert(donorTx[0]==1);donorConfig=uint16_t(donorTx[1])<<8|donorTx[2];}
    else assert(donorTx.size()==1&&donorTx[0]==0);
    return 0;
}
uint8_t TwoWire::requestFrom(uint8_t a,uint8_t n){assert(a==0x45&&n==2);return wireBytes=n;}
uint8_t TwoWire::available(){return wireBytes;}
unsigned TwoWire::readBytes(uint8_t *p,unsigned n){
    assert(n==2&&wireBytes==2);p[0]=sampleWord>>8;p[1]=sampleWord;wireBytes=0;return n;
}
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n){
    if(s->address==0x23){assert(n==1&&p[0]==0x86);return 0;}
    assert(s->address==0x45);
    if(n==1){selected=p[0];return int(selected)==failSelect?-EIO:0;}
    assert(n==3&&p[0]==1);++writes;
    auto word=uint16_t(p[1])<<8|p[2];if(word==failWrite)return -EIO;
    assert(word==0xc610||word==0xc010);config=word;return 0;
}
int i2c_read_dt(const i2c_dt_spec *s,uint8_t *p,size_t n){
    if(s->address==0x23){assert(n==1);*p=0x92;return 0;}
    assert(s->address==0x45&&n==2);++reads;
    if(int(selected)==failRead)return -EIO;
    uint16_t value;
    switch(selected){case 0:value=sampleWord;break;case 1:value=(config^uint16_t(corrupt?0x800:0))|flags;break;
        case 0x7e:value=maker;break;case 0x7f:value=part;break;default:assert(false);return -EIO;}
    p[0]=value>>8;p[1]=value;return 0;
}
int i2c_write_read_dt(const i2c_dt_spec *,const uint8_t*,size_t,uint8_t*,size_t){assert(false);return -EIO;}
ssize_t settings_get_val_len(const char*){return -ENOENT;}
ssize_t settings_load_one(const char*,void*,size_t){return -ENOENT;}
int settings_save_one(const char*,const void*,size_t){return -ENOTSUP;}
int main(){
    timing();worker();
    puts("PASS original OPT3001 and ClosedCube lux, 100ms ready/overflow, worker lifecycle, identity and bus faults, stock field priority and solicited wire");
}
