// SPDX-License-Identifier: GPL-3.0-only
#include "OpticalWindow.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#define MT_OPTICAL_NODE DT_ALIAS(zt_optical)
namespace mt::stock::telemetry::max30102 {
namespace {
static_assert(DT_NODE_HAS_COMPAT(MT_OPTICAL_NODE,maxim_max30102),"zt-optical requires a MAX30102");
static_assert(DT_PROP(MT_OPTICAL_NODE,smp_sr)==100 && DT_PROP(MT_OPTICAL_NODE,smp_ave)==4,
              "MAX30102 stock algorithm needs 25 effective samples per second");
static_assert(DT_ENUM_IDX(MT_OPTICAL_NODE,acq_mode)==2 && DT_PROP(MT_OPTICAL_NODE,led_pw)==411 &&
              DT_PROP(MT_OPTICAL_NODE,adc_rge)==4096, "Configure MAX30102 in stock SpO2 mode at 411 us, 4096 nA");
static_assert(DT_PROP_BY_IDX(MT_OPTICAL_NODE,led_pa,0)==60 && DT_PROP_BY_IDX(MT_OPTICAL_NODE,led_pa,1)==60,
              "Configure both LED pulse amplitudes for the stock optical profile");
const device *const sensor=DEVICE_DT_GET(MT_OPTICAL_NODE);
const i2c_dt_spec bus=I2C_DT_SPEC_GET(MT_OPTICAL_NODE);
OpticalWindow window;
bool active=false;
uint64_t lastPoll=0;
constexpr uint8_t FifoWritePointer=0x04,FifoOverflow=0x05,FifoReadPointer=0x06;
}
bool available(){return device_is_ready(sensor)&&i2c_is_ready_dt(&bus);}
void poll(bool enabled,uint64_t now){
    if(!enabled||!available()){active=false;window.reset();return;}
    if(active&&now>=lastPoll&&now-lastPoll<20)return;
    const bool late=now<lastPoll||now-lastPoll>=1000;
    lastPoll=now;
    uint8_t pointers[3]{};
    if(i2c_burst_read_dt(&bus,FifoWritePointer,pointers,sizeof(pointers))){active=false;window.reset();return;}
    const uint8_t newest=pointers[0]&31,oldest=pointers[2]&31;
    if(!active||late||pointers[1]){
        // Discard old, discontinuous or overflowed samples. Pointer reads
        // prevent Zephyr's raw fetch API from returning an empty FIFO slot.
        window.reset();active=!i2c_reg_write_byte_dt(&bus,FifoReadPointer,newest)&&!i2c_reg_write_byte_dt(&bus,FifoOverflow,0);
        return;
    }
    const unsigned pending=(32u+newest-oldest)%32u;
    for(unsigned i=0;i<pending&&i<4;++i){
        sensor_value red{},ir{};
        if(sensor_sample_fetch(sensor)||sensor_channel_get(sensor,SENSOR_CHAN_RED,&red)||sensor_channel_get(sensor,SENSOR_CHAN_IR,&ir)||
           red.val2||ir.val2||red.val1<0||ir.val1<0||red.val1>0x3ffff||ir.val1>0x3ffff){
            active=false;window.reset();return;
        }
        window.add(uint32_t(ir.val1),uint32_t(red.val1),now);
    }
}
bool sample(uint64_t now,meshtastic_HealthMetrics &out){return window.snapshot(now,out);}
}
#undef MT_OPTICAL_NODE
