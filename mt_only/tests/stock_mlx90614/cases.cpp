// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "telemetry/CompletionTelemetry.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "stock/telemetry/accessory/Mlx90614.h"
#include "oracle.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

using namespace mt::stock::telemetry::accessory;
namespace {
using Service = mt::telemetry::CompletionTelemetry;
using Result = Service::Result;
int oracleWrites=0, targetTransfers=0;
bool ready=true;
// Polynomial long division is independent of the candidate's shift implementation.
uint8_t pec(const std::vector<uint8_t> &bytes) {
    unsigned r=0;
    for (unsigned b:bytes) {
        r=(r<<8)|b;
        for (int shift=7;shift>=0;--shift) if(r&(0x100u<<shift))r^=0x107u<<shift;
    }
    r<<=8;
    for(int shift=7;shift>=0;--shift) if(r&(0x100u<<shift))r^=0x107u<<shift;
    return r;
}
struct Chip {
    std::array<uint16_t,64> regs{};
    int nack=-1,badPec=-1;
    Chip(){regs[0x2e]=0x5a;regs[0x3c]=0xa5a5;regs[0x3d]=1;regs[0x3e]=2;regs[0x3f]=3;regs[0x24]=64224;regs[6]=14658;regs[7]=15474;}
    bool get(unsigned r,uint8_t *bytes,size_t n){
        assert(r<regs.size()&&n==3);if(int(r)==nack)return false;
        bytes[0]=uint8_t(regs[r]);bytes[1]=regs[r]>>8;
        bytes[2]=pec({0xb4,uint8_t(r),0xb5,bytes[0],bytes[1]})^(r==unsigned(badPec));
        return true;
    }
} oracleChip, target;
void tick(unsigned count=1){
    mockKernel::timerTicks=count;
    try{mockKernel::accessoryThread(nullptr,nullptr,nullptr);}catch(mockKernel::NoJob &){}
}
meshtastic_Telemetry decode(const uint8_t *bytes,size_t n,unsigned request){
    meshtastic_Data d{};auto s=pb_istream_from_buffer(bytes,n);
    assert(pb_decode(&s,meshtastic_Data_fields,&d)&&d.request_id==request);
    meshtastic_Telemetry out{};s=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&s,meshtastic_Telemetry_fields,&out)&&out.which_variant==meshtastic_Telemetry_health_metrics_tag);return out;
}
void test() {
    static_assert(offsetof(meshtastic_EnvironmentMetrics,has_temperature)==offsetof(meshtastic_HealthMetrics,has_heart_bpm));
    static_assert(offsetof(meshtastic_EnvironmentMetrics,temperature)!=offsetof(meshtastic_HealthMetrics,temperature));
    Service svc; meshtastic_ModuleConfig_TelemetryConfig cfg{};cfg.health_measurement_enabled=true;
    ready=false;assert(!svc.configSupported(cfg));ready=true;assert(svc.configSupported(cfg));
    tick(2);assert(targetTransfers==0);enableEnvironment(true);tick(2);assert(!targetTransfers);
    assert(svc.configure(cfg));assert(mlx90614Sensor.runOnce()==0 && oracleWrites==0);
    HealthTelemetryModule original;
    uint8_t bytes[237];size_t n=0;
    for(uint16_t raw:{1,1000,13657,14235,14987,15513,27107,32767}) {
        for(bool optical:{false,true}) {
            target.regs[7]=oracleChip.regs[7]=raw;
            target.regs[6]=oracleChip.regs[6]=uint16_t(raw^0x0555);
            max30102Sensor.present=optical;
            meshtastic_Telemetry donor{};
            assert(original.getHealthTelemetry(&donor));
            assert(donor.which_variant==meshtastic_Telemetry_health_metrics_tag);
            auto &expected=donor.variant.health_metrics;
            assert(expected.has_temperature && expected.has_heart_bpm && expected.heart_bpm==(optical?71:0));
            assert(expected.has_spO2==optical);
            uint8_t oracleWire[237];
            auto stream=pb_ostream_from_buffer(oracleWire,sizeof(oracleWire));
            assert(pb_encode(&stream,meshtastic_Telemetry_fields,&donor));
            auto input=pb_istream_from_buffer(oracleWire,stream.bytes_written);
            meshtastic_Telemetry donorWire{};assert(pb_decode(&input,meshtastic_Telemetry_fields,&donorWire));
            assert(donorWire.which_variant==meshtastic_Telemetry_health_metrics_tag &&
                   donorWire.variant.health_metrics.has_heart_bpm && donorWire.variant.health_metrics.heart_bpm==(optical?71:0));
            assert(donorWire.variant.health_metrics.temperature==expected.temperature);
            tick();auto now=k_uptime_get();assert(svc.poll(now)==Result::Ok);
            Service::Snapshot observed{};assert(svc.snapshot(now,observed)==Result::Ok && observed.hasHealth && !observed.hasAir);
            assert(observed.health.has_temperature && observed.health.temperature==expected.temperature);
            assert(!observed.health.has_heart_bpm && !observed.health.has_spO2);
            assert(svc.response(meshtastic_Telemetry_health_metrics_tag,42,now,73,bytes,sizeof(bytes),n)==Result::Ok);
            auto reply=decode(bytes,n,42);assert(reply.time==73&&reply.variant.health_metrics.temperature==expected.temperature);
            assert(!reply.variant.health_metrics.has_heart_bpm);
            meshtastic_HealthMetrics genuine{};genuine.has_heart_bpm=genuine.has_spO2=true;genuine.heart_bpm=71;genuine.spO2=96;
            assert(objectTemperature(now,genuine)&&genuine.temperature==expected.temperature&&genuine.heart_bpm==71&&genuine.spO2==96);
        }
    }
    zt::telemetry::EnvironmentReadings readings;
    readings.accessory=environment(k_uptime_get());
    assert(!zt::telemetry::toMeshtasticEnvironmentMetrics(readings).has_temperature);
    auto check=[&](bool valid){tick();assert((svc.poll(k_uptime_get())==Result::Ok)==valid);};
    for(int field:{0x24,0x07}){
        target.nack=field;check(false);target.nack=-1;check(true);
        target.badPec=field;check(false);target.badPec=-1;check(true);
    }
    for(uint16_t bad:{0,0x8000,0xffff}){target.regs[7]=bad;check(false);}
    target.regs[7]=15474;check(true);
    for(uint16_t setting:{0,0xffff,64158,64290}){target.regs[0x24]=setting;check(false);}
    for(uint16_t setting:{64159,64224,64289}){target.regs[0x24]=setting;check(true);}
    const auto now=k_uptime_get();meshtastic_HealthMetrics h{};
    assert(!objectTemperature(now+90001,h));
    cfg.health_measurement_enabled=false;assert(svc.configure(cfg));const int before=targetTransfers;tick(2);
    assert(before==targetTransfers&&!objectTemperature(k_uptime_get(),h));
    cfg.health_measurement_enabled=true;assert(svc.configure(cfg));
    target.regs[0x2e]=0x15;check(false);target.regs[0x2e]=0x5a;
    target.nack=0x3c;check(false);target.nack=-1;target.badPec=0x3f;check(false);target.badPec=-1;
    for(unsigned r=0x3c;r<=0x3f;++r)target.regs[r]=0xffff;
    check(false);target.regs[0x3c]=123;check(true);
    // Stock erases and programs the chip on a mismatch, but its API cannot report write failures.
    oracleChip.regs[0x24]=0xffff;assert(mlx90614Sensor.runOnce()==0 && oracleWrites==2 && oracleChip.regs[0x24]==64224);
    puts("PASS pinned health module + MLX90614 + Adafruit donor, object values, ambient union alias, optical preservation and phone wire");
    puts("PASS DT readiness, signature/PEC, configuration and lifecycle, no EEPROM writes, invalid emissivity or object error flag suppresses measurement");
}
}
TwoWire Wire;
bool TwoWire::readReg(uint8_t r,uint8_t *p,size_t n){return oracleChip.get(r,p,n);}
bool TwoWire::write(const uint8_t *p,size_t n) {
    assert(n==4&&p[0]==0x24&&p[3]==pec({0xb4,p[0],p[1],p[2]}));++oracleWrites;
    oracleChip.regs[0x24]=uint16_t(p[2])<<8|p[1];return true;
}
void delay(unsigned long n){assert(n==10);}
bool i2c_is_ready_dt(const i2c_dt_spec *s){assert(s->address==0x5a);return ready;}
int i2c_write_dt(const i2c_dt_spec *,const uint8_t *,size_t){assert(false);return -EIO;}
int i2c_read_dt(const i2c_dt_spec *,uint8_t *,size_t){assert(false);return -EIO;}
int i2c_write_read_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n,uint8_t *out,size_t count){
    assert(s->address==0x5a&&n==1);++targetTransfers;return target.get(*p,out,count)?0:-EIO;
}
bool device_is_ready(const device *){return false;}
int sensor_sample_fetch(const device *){assert(false);return -ENODEV;}
int sensor_channel_get(const device *,sensor_channel,sensor_value *){assert(false);return -ENODEV;}
ssize_t settings_get_val_len(const char *){return -ENOENT;}
ssize_t settings_load_one(const char *,void *,size_t){return -ENOENT;}
int settings_save_one(const char *,const void *,size_t){return -ENOTSUP;}
int main(){test();}
