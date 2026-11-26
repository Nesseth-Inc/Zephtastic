// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentQuery.h"
#include "telemetry/CompletionTelemetry.h"
#include "stock/telemetry/accessory/Accessories.h"
#include "stock/telemetry/SensorModules.h"
#include "oracle.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <array>
#include <vector>
#include <cassert>
#include <cstdio>
using namespace mt::stock::telemetry::accessory;
using mt::telemetry::CompletionTelemetry;
namespace {
struct Model {
    std::array<int16_t,3> air{123,-5,-3123};
    uint16_t dry=510, wet=390, raw=420, temp=218, command=0;
    uint8_t version=3, percent=77, reg=0;
    unsigned operations=0, failed=0, resetWrites=0, calWrites=0, wakePulses=0;
    bool corrupt=false, ignoreCalibration=false;
    uint64_t selectedAt=0;
    int write(uint8_t addr,const uint8_t *p,size_t n,uint64_t now) {
        if (++operations==failed) return -EIO;
        selectedAt=now;
        if (addr==0x5d) {
            assert(n==2);
            command=(uint16_t(p[0])<<8)|p[1];
            assert(command==0x0327 || command==0xd304 || command==6 || command==0x0104);
            resetWrites+=command==0xd304;
        } else {
            assert(addr==0x20);
            reg=p[0];
            if (n==3) {
                assert(reg==7||reg==8);++calWrites;
                if(!ignoreCalibration)(reg==7?wet:dry)=(uint16_t(p[1])<<8)|p[2];
            } else if(n==2) { assert(reg==6&&p[1]==0); }
            else assert(n==1);
        }
        return 0;
    }
    int read(uint8_t addr,uint8_t *p,size_t n,uint64_t now) {
        if (++operations==failed) return -EIO;
        if (addr==0x5d) {
            assert(n==9&&command==0x0327&&now-selectedAt>=5);
            for(unsigned i=0;i<3;++i) {
                p[3*i]=uint16_t(air[i])>>8;p[3*i+1]=uint8_t(air[i]);p[3*i+2]=Bus::crc(p+i*3);
            }
            if(corrupt)p[5]^=1;
        } else {
            assert(addr==0x20&&now-selectedAt>=20);
            if(n==1) { assert(reg==4||reg==9);*p=reg==4?version:percent; }
            else {
                assert(n==2&&(reg==0x0a||reg==0x0b||reg==1||reg==5));
                uint16_t v=reg==0x0a?wet:reg==0x0b?dry:reg==1?raw:temp;
                p[0]=v>>8;p[1]=v;
            }
        }
        return 0;
    }
};
Model workerModel, donorModel;
uint64_t donorMs=0;
uint8_t wireAddress=0;std::vector<uint8_t> tx,rx;size_t rxUsed=0;
struct Port:Bus {
    uint8_t addr;Model &model;uint64_t ms=0;
    Port(Model &m,uint8_t a):addr(a),model(m){}
    int write(const uint8_t *p,size_t n)override{return model.write(addr,p,n,ms);}
    int read(uint8_t *p,size_t n)override{return model.read(addr,p,n,ms);}
    int readReg(uint8_t,uint8_t *,size_t)override{assert(false);return -EIO;}
    void wait(unsigned dt)override{ms+=dt;}uint64_t now()override{return ms;}
    uint32_t utc()override{return 0;}
    int load(const char *,void *,size_t)override{return -ENOENT;}
    int save(const char *,const void *,size_t)override{return -ENOTSUP;}
};
int resetOk(bool){return 0;}
void checkAir() {
    for (auto values : {std::array<int16_t,3>{0,0,0},{1,9999,-401},{-3,10000,2456},{32767,-32768,32767}}) {
        donorModel=Model{};donorModel.air=values;
        Model m=donorModel;
        SFA30Sensor donor;donor.sfa30.begin(Wire,0x5d);
        meshtastic_Telemetry expected{};assert(donor.getMetrics(&expected));
        Port b(m,0x5d);Sfa30 sensor(b);Formaldehyde result;
        assert(sensor.measure(result)==-EAGAIN&&m.command==0xd304);
        b.wait(999);assert(sensor.measure(result)==-EAGAIN&&m.command==0xd304);
        b.wait(1);assert(sensor.measure(result)==-EAGAIN&&m.command==6);
        b.wait(9999);assert(sensor.measure(result)==-EAGAIN&&m.command==6);
        b.wait(1);assert(!sensor.measure(result));
        auto &air=expected.variant.air_quality_metrics;
        assert(air.has_form_formaldehyde&&air.has_form_temperature&&air.has_form_humidity);
        assert(result.ppb==air.form_formaldehyde&&result.celsius==air.form_temperature&&result.humidity==air.form_humidity);
        m.corrupt=true;assert(sensor.measure(result)==-EIO);
        sensor.stop();assert(m.command==0x0104);
        m.corrupt=false;assert(sensor.measure(result)==-EAGAIN);b.wait(1000);
        assert(sensor.measure(result)==-EAGAIN);b.wait(10000);assert(!sensor.measure(result));
        m.corrupt=true;for(int retry=0;retry<3;++retry)assert(sensor.measure(result)==-EIO);
        assert(sensor.measure(result)==-EAGAIN&&m.command==0xd304);
    }
    puts("PASS pinned SFA30 and Sensirion-Core byte decoding, signed HCHO/RH/temperature, reset, warm-up, CRC, stop");
}
void checkSoil() {
    for(uint8_t v:{1,2,3,9}) for(uint16_t raw:{0,389,390,391,419,450,509,510,511,65535}) {
        donorModel=Model{};donorModel.raw=raw;donorModel.version=v;donorModel.temp=raw;donorModel.percent=raw%101;
        Model m=donorModel;
        RAK12035Sensor donor;uint16_t cal=0;uint8_t version=0;
        assert(donor.sensor.get_sensor_version(&version)&&donor.sensor.get_dry_cal(&cal)&&donor.sensor.get_wet_cal(&cal));
        meshtastic_Telemetry expect{};assert(donor.getMetrics(&expect));
        Port b(m,0x20);Rak12035 sensor(b,resetOk);
        const auto got=sensor.measure();auto &expected=expect.variant.environment_metrics;
        assert(got.valid&&got.moisture==expected.soil_moisture&&got.celsius==expected.soil_temperature);
        auto calls=m.operations;assert(sensor.measure().valid&&calls==m.operations);
    }
    Model m;Port b(m,0x20);Rak12035 sensor(b,resetOk);
    m.dry=m.wet=0;m.version=2;
    assert(sensor.measure().valid&&m.dry==510&&m.wet==390&&m.calWrites==2);
    sensor.stop();m.dry=m.wet=390;
    assert(!sensor.measure().valid&&m.calWrites==2);
    sensor.stop();m.dry=0;m.ignoreCalibration=true;
    assert(!sensor.measure().valid);
    m.ignoreCalibration=false;sensor.stop();m.version=0;
    assert(!sensor.measure().valid);m.version=3;b.wait(300);assert(sensor.measure().valid);
    // A NACK on any calibration/read/sleep operation suppresses the pair.
    Model good;Port goodBus(good,0x20);Rak12035 goodSensor(goodBus,resetOk);
    assert(goodSensor.measure().valid);
    for(unsigned f=1;f<=good.operations;++f){
        Model bad;bad.failed=f;Port port(bad,0x20);Rak12035 failed(port,resetOk);
        assert(!failed.measure().valid);
    }
    puts("PASS 40 RAK12035 1.0.4/stock wire differentials, capacitance clamps, direct v3 percent, calibration readback, NACK and awake retry");
}
void tick(unsigned ticks=1){mockKernel::timerTicks=ticks;try{mockKernel::accessoryThread(nullptr,nullptr,nullptr);}catch(mockKernel::NoJob &){}}
meshtastic_Telemetry decode(const uint8_t *p,size_t n,unsigned id) {
    meshtastic_Data d{};auto in=pb_istream_from_buffer(p,n);
    assert(pb_decode(&in,meshtastic_Data_fields,&d)&&d.request_id==id);
    meshtastic_Telemetry t{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
    assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));return t;
}
void integration() {
    CompletionTelemetry service;meshtastic_ModuleConfig_TelemetryConfig cfg{};
    tick();assert(!workerModel.operations);
    cfg.air_quality_enabled=true;assert(service.configSupported(cfg)&&service.configure(cfg));enableEnvironment(true);
    tick(15);auto now=k_uptime_get();
    assert(service.poll(now)==CompletionTelemetry::Result::Ok);
    CompletionTelemetry::Snapshot snap{};assert(service.snapshot(now,snap)==CompletionTelemetry::Result::Ok);
    meshtastic_Telemetry report{};const unsigned air=meshtastic_Telemetry_air_quality_metrics_tag;
    assert(mt::stock::telemetry::allocReply(air,snap,17,report));
    uint8_t bytes[237];size_t n=0;
    assert(mt::stock::telemetry::encodeReport(report,0,bytes,sizeof(bytes),n));
    assert(decode(bytes,n,0).variant.air_quality_metrics.form_temperature==workerModel.air[2]/200.f);
    assert(service.response(air,43,now,18,bytes,sizeof(bytes),n)==CompletionTelemetry::Result::Ok);
    auto a=decode(bytes,n,43).variant.air_quality_metrics;
    assert(a.has_form_formaldehyde&&a.form_formaldehyde==workerModel.air[0]/5.f&&a.has_form_humidity&&a.has_form_temperature);
    zt::telemetry::EnvironmentReadings sample;sample.accessory=environment(now);
    assert(sample.accessory.soil.valid&&mt::stock::telemetry::environmentReport(report,18,sample));
    assert(mt::EnvironmentQuery::responseMetrics(report.variant.environment_metrics,21,bytes,sizeof(bytes),n));
    auto e=decode(bytes,n,21).variant.environment_metrics;
    assert(e.has_soil_moisture&&e.soil_moisture==77&&e.has_soil_temperature&&e.soil_temperature==21.8f);
    assert(!environment(sample.accessory.soil.at+90001).soil.valid);
    workerModel.corrupt=true;tick();assert(service.poll(k_uptime_get())==CompletionTelemetry::Result::Unavailable);
    assert(environment(k_uptime_get()).soil.valid);
    enableEnvironment(false);cfg.air_quality_enabled=false;assert(service.configure(cfg));tick();assert(!environment(k_uptime_get()).soil.valid);
    const auto count=workerModel.operations;tick(3);assert(count==workerModel.operations);
    puts("PASS worker ownership, independent caches, disable and physical sample age, complete Air/Environment periodic and phone wire");
}
}
TwoWire Wire;
void TwoWire::beginTransmission(uint8_t a){wireAddress=a;tx.clear();}
size_t TwoWire::write(const uint8_t *p,size_t n){tx.insert(tx.end(),p,p+n);return n;}
size_t TwoWire::write(uint8_t x){return write(&x,1);}
uint8_t TwoWire::endTransmission(){return donorModel.write(wireAddress,tx.data(),tx.size(),donorMs)?2:0;}
uint8_t TwoWire::requestFrom(uint8_t a,uint8_t n,bool stop){assert(stop);rx.resize(n);rxUsed=0;return donorModel.read(a,rx.data(),n,donorMs)?0:n;}
size_t TwoWire::available(){return rx.size()-rxUsed;}
int TwoWire::read(){assert(available());return rx[rxUsed++];}
void delay(unsigned dt){donorMs+=dt;} unsigned long millis(){return donorMs;}
void digitalWrite(unsigned,unsigned){} void pinMode(unsigned,unsigned){}
bool device_is_ready(const device *){return false;}int sensor_sample_fetch(const device *){return -ENODEV;}
int sensor_channel_get(const device *,sensor_channel,sensor_value *){return -ENODEV;}
bool gpio_is_ready_dt(const gpio_dt_spec *){return true;}
int gpio_pin_configure_dt(const gpio_dt_spec *,int f){workerModel.wakePulses+=f==GPIO_OUTPUT_ACTIVE;return 0;}
bool i2c_is_ready_dt(const i2c_dt_spec *){return true;}
int i2c_write_dt(const i2c_dt_spec *s,const uint8_t *p,size_t n){return workerModel.write(s->address,p,n,k_uptime_get());}
int i2c_read_dt(const i2c_dt_spec *s,uint8_t *p,size_t n){return workerModel.read(s->address,p,n,k_uptime_get());}
int i2c_write_read_dt(const i2c_dt_spec *,const uint8_t *,size_t,uint8_t *,size_t){assert(false);return -EIO;}
ssize_t settings_get_val_len(const char *){return -ENOENT;}
ssize_t settings_load_one(const char *,void *,size_t){return -ENOENT;}
int settings_save_one(const char *,const void *,size_t){return -ENOTSUP;}
int main(){checkAir();checkSoil();integration();}
