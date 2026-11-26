// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "bh-oracle.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace mt::stock::telemetry::accessory;
namespace {
std::vector<unsigned> donorCmd,bhCmd;
uint16_t bhRaw=0;uint8_t bhId=0x11;
int bhFault=-1;bool shortRead=false;unsigned bhReads=0,donorN=0;
bool bhOn=false;uint64_t bhAt=0;
void tick(unsigned n=1){mockKernel::timerTicks=n;try{mockKernel::accessoryThread(nullptr,nullptr,nullptr);}catch(const mockKernel::NoJob &){}}
void test(){
    tick();assert(bhCmd.empty());enableEnvironment(true);tick(3);
    BH1750Sensor oracle; ScanI2C::FoundDevice device;
    assert(oracle.initDevice(&Wire,&device));
    assert((donorCmd==std::vector<unsigned>{0x10,0x10,0x42,0x65,0x10}));
    assert((bhCmd==std::vector<unsigned>{0x86,1,0x10,0x10,0x42,0x65,0x10}));
    const size_t commands=bhCmd.size();
    for(uint16_t word:{0,1,2,3,555,16384,32767,65535}){
        bhRaw=word;tick();
        meshtastic_Telemetry expected{};assert(oracle.getMetrics(&expected));
        auto e=environment(k_uptime_get());assert(e.hasBhLux && e.bhLux==expected.variant.environment_metrics.lux);
        zt::telemetry::EnvironmentReadings r;r.accessory=e;r.accessory.hasLight=true;r.accessory.lux=72;r.accessory.uvIndex=3;
        meshtastic_Telemetry t{};assert(mt::stock::telemetry::environmentReport(t,4,r));
        const auto &m=t.variant.environment_metrics;assert(m.has_lux&&m.lux==e.bhLux&&m.has_uv_lux&&m.uv_lux==3);
        uint8_t bytes[237];size_t n=0;assert(mt::EnvironmentQuery::responseMetrics(m,7,bytes,sizeof(bytes),n));
        meshtastic_Data data{};auto in=pb_istream_from_buffer(bytes,n);assert(pb_decode(&in,meshtastic_Data_fields,&data)&&data.request_id==7);
        meshtastic_Telemetry reply{};in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
        assert(pb_decode(&in,meshtastic_Telemetry_fields,&reply)&&reply.variant.environment_metrics.has_lux&&reply.variant.environment_metrics.lux==m.lux);
    }
    assert(bhCmd.size()==commands&&bhOn);shortRead=true;tick();assert(!environment(k_uptime_get()).hasBhLux&&!bhOn);
    shortRead=false;bhId=0x92;tick(3);assert(!bhOn&&!environment(k_uptime_get()).hasBhLux);
    bhId=0;bhFault=0x42;tick(3);assert(!bhOn&&!environment(k_uptime_get()).hasBhLux);
    bhFault=-1;tick(4);assert(bhOn&&environment(k_uptime_get()).hasBhLux);
    enableEnvironment(false);tick();assert(!bhOn&&!environment(k_uptime_get()).hasBhLux);
    enableEnvironment(true);tick(4);assert(environment(k_uptime_get()).hasBhLux);
    assert(!environment(k_uptime_get()+90001).hasBhLux);
    puts("PASS BH1750 donor continuous commands and lux, worker and phone fields, partial startup, colliding LTR553, short read, disable and recovery");
}
}
TwoWire Wire;
void delay(unsigned long n){assert(n==200);}
void TwoWire::beginTransmission(uint8_t a){assert(a==0x23);}
void TwoWire::write(uint8_t c){donorCmd.push_back(c);}
uint8_t TwoWire::endTransmission(bool){return 0;}
uint8_t TwoWire::requestFrom(uint8_t a,uint8_t n){assert(a==0x23&&n==2);donorN=2;return n;}
uint8_t TwoWire::available(){return donorN;}
uint8_t TwoWire::read(){assert(donorN);return --donorN?bhRaw>>8:uint8_t(bhRaw);}
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n){
    assert(s->address==0x23&&n==1);bhCmd.push_back(*p);if(*p==bhFault)return -EIO;
    if(*p==1||*p==0x10){bhOn=true;bhAt=k_uptime_get();}if(*p==0)bhOn=false;
    return 0;
}
int i2c_read_dt(const i2c_dt_spec *s,uint8_t *p,size_t n){
    assert(s->address==0x23);if(n==1){assert(bhCmd.back()==0x86);*p=bhId;return 0;}
    assert(n==2&&bhOn&&k_uptime_get()-bhAt>=180);++bhReads;
    if(shortRead)return -EIO;p[0]=bhRaw>>8;p[1]=bhRaw;return 0;
}
int i2c_write_read_dt(const i2c_dt_spec *,const uint8_t*,size_t,uint8_t*,size_t){assert(false);return -EIO;}
ssize_t settings_get_val_len(const char*){return -ENOENT;}
ssize_t settings_load_one(const char*,void*,size_t){return -ENOENT;}
int settings_save_one(const char*,const void*,size_t){return -ENOTSUP;}
int main(){test();}
