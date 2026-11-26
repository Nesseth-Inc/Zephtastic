// SPDX-License-Identifier: GPL-3.0-only
#include "driver_fixture.h"
#include "power/InaTelemetry.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <vector>
static uint16_t regs[256]{};
static int failReg=-1;static bool failWrite,ready=true;static unsigned waited,writes;
static std::vector<unsigned> reads;
extern "C" bool i2c_is_ready_dt(const i2c_dt_spec *b){assert(b->tag==1);return ready;}
extern "C" int i2c_write_read_dt(const i2c_dt_spec *b,const void *tx,size_t n,void *rx,size_t cap){
    assert(b->tag==1&&n==1&&cap==2);unsigned reg=*static_cast<const uint8_t*>(tx);reads.push_back(reg);
    if(int(reg)==failReg)return -EIO;sys_put_be16(regs[reg],static_cast<uint8_t*>(rx));return 0;
}
extern "C" int i2c_write_dt(const i2c_dt_spec *b,const void *tx,size_t n){
    assert(b->tag==1&&n==3);const auto *d=static_cast<const uint8_t*>(tx);
    assert(d[0]==0&&sys_get_be16(d+1)==(regs[0]|0x8000));++writes;return failWrite?-EIO:0;
}
extern "C" void k_msleep(int ms){assert(ms==2);waited+=ms;}
enum {INA260_REG_CURRENT=1,INA260_REG_BUSVOLTAGE=2,MSBFIRST};
struct Adafruit_I2CRegister {unsigned reg;Adafruit_I2CRegister(void*,unsigned r,int,int):reg(r){}uint16_t read(){return regs[reg];}};
struct Adafruit_INA260 {void *i2c_dev=nullptr;float readCurrent();float readBusVoltage();};
#include "vendor-values.inc"
int main(){
    ready=false;assert(testInit()==-ENODEV&&reads.empty());ready=true;
    assert(testInit()==-ENODEV&&reads==std::vector<unsigned>{0xfe});regs[0xfe]=0x5449;
    regs[0xff]=0x2200;assert(testInit()==-ENODEV&&!writes);regs[0xff]=0x227f;regs[0]=0x6127;
    for(int fail:{0xfe,0xff,0}){failReg=fail;assert(testInit()==-EIO&&!writes&&!waited);}
    failReg=-1;failWrite=true;assert(testInit()==-EIO&&writes==1&&!waited);failWrite=false;reads.clear();
    assert(!testInit()&&reads==(std::vector<unsigned>{0xfe,0xff,0})&&writes==2&&waited==2);
    Adafruit_INA260 donor;struct sensor_value value;
    for(unsigned raw=0;raw<65536;++raw){
        regs[1]=raw;regs[2]=raw;
        assert(!testApi->sample_fetch(&testDevice,SENSOR_CHAN_ALL));
        assert(!testApi->channel_get(&testDevice,SENSOR_CHAN_CURRENT,&value));
        assert(value.val1*1000000+value.val2==int32_t(double(donor.readCurrent())*1000));
        env_sensor_reading sample{};sample.fields=ENV_F_CURRENT|ENV_F_VOLTAGE;
        sample.current_a=value.val1+value.val2/1000000.f;
        assert(!testApi->channel_get(&testDevice,SENSOR_CHAN_VOLTAGE,&value));
        assert(value.val1*1000000+value.val2==int32_t(double(donor.readBusVoltage())*1000));
        sample.voltage_v=value.val1+value.val2/1000000.f;
        const auto wire=mt::power::ina260Channel(sample);
        assert(*wire.current==donor.readCurrent()&&*wire.voltage==donor.readBusVoltage()/1000.f);
    }
    for(int failure:{1,2}){
        failReg=failure;assert(!testApi->sample_fetch(&testDevice,SENSOR_CHAN_ALL));
        assert(testApi->channel_get(&testDevice,SENSOR_CHAN_CURRENT,&value)==(failure==1?-ENODATA:0));
        assert(testApi->channel_get(&testDevice,SENSOR_CHAN_VOLTAGE,&value)==(failure==2?-ENODATA:0));
    }
    assert(testApi->sample_fetch(&testDevice,SENSOR_CHAN_LIGHT)==-ENOTSUP);
    puts("PASS INA260 identity and reset framing, failed transport, independent channel validity, all 65536 signed current and unsigned voltage conversions vs pinned Adafruit");
}
