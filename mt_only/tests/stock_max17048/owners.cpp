// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrHardware.h"
#include "power/InaBatterySelector.h"
#include "power/Max17048.h"
#include "mock_control.h"
#include <cassert>
#include <cerrno>
#include <cstdio>

namespace mt::power::max17048 {
static Sample owner;
struct Registers {
    uint16_t words[32]{};
    bool ready=false;
    bool word(uint8_t reg,uint16_t &out){out=words[reg];return ready;}
    bool set(uint8_t reg,uint16_t value){words[reg]=value;return ready;}
} bus;
void poll(uint64_t now){owner.poll(bus,now);}
bool identified(){return owner.identified();}
Reading snapshot(uint64_t now){return owner.get(now);}
}
namespace mt::power::ina {
static uint32_t selected=0;
bool configure(uint32_t address){selected=address;return address==0||address==0x40;}
void poll(uint64_t){assert(selected==0x40);}
Snapshot snapshot(uint64_t now){return {Status::Fresh,selected,3.3f,0.2f,now};}
}

static meshtastic_DeviceMetrics tick(uint64_t now){
    mock.now=now;mt::hardware::poll(now);
    meshtastic_DeviceMetrics out{};mt::hardware::snapshot(now).apply(out);return out;
}
int main(){
    namespace gauge=mt::power::max17048;
    mock_reset();mt::hardware::init();
    assert(!tick(0).has_voltage);
    const auto initial=tick(10);
#if MT_MOCK_HAS_ADC
    assert(initial.has_voltage&&initial.voltage==3.895f&&mock.enable_calls==1&&!mock.refs);
#else
    assert(!initial.has_voltage&&!initial.has_battery_level&&!mock.enable_calls);
#endif
    tick(10000);
    gauge::bus.ready=true;gauge::bus.words[8]=0x13;gauge::bus.words[2]=53760;gauge::bus.words[4]=50*256;
    gauge::bus.words[0x16]=100;
    // The ADC divider is settling when the accessory first becomes reachable.
    mock.disable_status=-EIO;
    auto value=tick(11000);assert(value.has_voltage&&value.voltage==4.2f&&value.battery_level==50);
    mock.disable_status=0;tick(11001);
    const auto adcCalls=mock.enable_calls;
#if MT_MOCK_HAS_ADC
    assert(!mock.refs&&mock.enable_calls==2&&mock.disable_calls==3&&mock.read_calls==8);
#endif
    // Preserve the selected measurement owner if the gauge bus later goes away.
    gauge::bus.ready=false;value=tick(12000);assert(gauge::identified()&&!value.has_voltage);
    tick(12010);assert(mock.enable_calls==adcCalls);
    // An explicitly configured external INA owns DeviceMetrics. The independently
    // sampled MAX still supplies PowerMetrics channel one, with unrelated fields intact.
    assert(mt::hardware::setInaBatteryAddress(0x40));
    gauge::bus.ready=true;value=tick(13000);
    assert(value.has_voltage&&value.voltage==3.3f&&value.has_battery_level&&value.battery_level!=50);
    zt::telemetry::PowerReadings power;power.current[0]=0.2f;gauge::snapshot(13000).power(power);
    assert(power.voltage[0]&&*power.voltage[0]==4.2f&&power.current[0]&&*power.current[0]==0.2f);
    assert(mt::hardware::setInaBatteryAddress(0));value=tick(13001);
    assert(value.has_voltage&&value.voltage==4.2f&&value.battery_level==50);
    puts("PASS production board battery source selection, interrupted divider release, gauge outage, explicit INA and independent power channel");
}
