// SPDX-License-Identifier: GPL-3.0-only
// Production ZephyrHardware.cpp is a separate translation unit; no private-state hooks.
#include "ZephyrHardware.h"
#include "power/InaBatterySelector.h"
#include "mock_control.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <cerrno>
// This harness exercises the stock boards without an external INA.
namespace mt::power::ina {
bool configure(uint32_t address){return address==0;}
void poll(uint64_t){assert(false);}
Snapshot snapshot(uint64_t){assert(false);return {};}
}
static void tick(uint64_t now){mock.now=now;mt::hardware::poll(now);}
static meshtastic_DeviceMetrics metrics(uint64_t now){meshtastic_DeviceMetrics m{};mt::hardware::snapshot(now).apply(m);return m;}

#if MT_MOCK_HAS_ADC
static void emptyCell(uint64_t now){const auto s=mt::hardware::snapshot(now);assert(!s.cellMilliVolts&&!s.cellPercent);const auto m=metrics(now);assert(!m.has_voltage);}
static void raw(int16_t value){for(auto &v:mock.raw)v=value;}
static void burst(uint64_t start){tick(start);tick(start+9);assert(mock.sequence_calls%8==0);tick(start+10);}

#endif
int main(int argc,char **argv){assert(argc==2);std::cout.setf(std::ios::unitbuf);const std::string name=argv[1];mock_reset();
#if !MT_MOCK_HAS_ADC
 assert(name=="unsupported");mock.usb=true;mt::hardware::init();tick(0);tick(10);tick(10000);const auto s=mt::hardware::snapshot(10000);assert(!s.cellMilliVolts&&!s.cellPercent&&!s.externalPower);const auto m=metrics(10000);assert(!m.has_voltage&&!m.has_battery_level);assert(!mock.setup_calls&&!mock.enable_calls&&!mock.sequence_calls&&!mock.read_calls&&!mock.disable_calls&&!mock.vbus_calls);
#else
 if(name=="adc_not_ready")mock.adc_ready=false;
 if(name=="divider_not_ready")mock.divider_ready=false;
 if(name=="setup_error")mock.setup_status=-EIO;
 mt::hardware::init();
 if(name=="adc_not_ready"||name=="divider_not_ready"||name=="setup_error"){
  mock.usb=true;burst(0);emptyCell(10);assert(metrics(10).has_battery_level&&metrics(10).battery_level==101);assert(!mock.enable_calls&&!mock.read_calls&&!mock.disable_calls);assert(mock.setup_calls==(name=="setup_error"?1u:0u));
 }else if(name=="adc_divider_override"){
  raw(900);mt::hardware::setAdcMultiplierOverride(4.916f);burst(0);
  auto sample=mt::hardware::snapshot(10);
  assert(sample.cellMilliVolts&&*sample.cellMilliVolts==3888);
  auto report=metrics(10);assert(report.has_voltage&&report.voltage==3.888f);
  mt::hardware::setAdcMultiplierOverride(4.0f);burst(10000);
  sample=mt::hardware::snapshot(10010);assert(sample.cellMilliVolts&&*sample.cellMilliVolts==3164);
  mock.conversion_status=-ENOTSUP;burst(20000);emptyCell(20010);
  mt::hardware::setAdcMultiplierOverride(0);burst(30000);
  sample=mt::hardware::snapshot(30010);assert(sample.cellMilliVolts&&*sample.cellMilliVolts==3895);
 }else if(name=="success_and_cadence"){
  const int16_t samples[8]={899,901,899,901,900,900,900,900};for(unsigned i=0;i<8;++i)mock.raw[i]=samples[i];
  tick(0);assert(mock.enable_calls==1&&!mock.read_calls&&mock.refs==1);emptyCell(0);tick(9);assert(!mock.read_calls&&mock.refs==1);tick(10);
  const auto s=mt::hardware::snapshot(10);assert(s.cellMilliVolts&&*s.cellMilliVolts==3895&&s.cellPercent&&*s.cellPercent==70&&s.externalPower&&!*s.externalPower&&s.capturedMs==10);
  assert(mock.sequence_calls==8&&mock.read_calls==8&&mock.disable_calls==1&&mock.refs==0&&mock.setup_calls==1);for(unsigned i=0;i<8;++i)assert(mock.read_times[i]==10);
  const auto m=metrics(10);assert(m.has_voltage&&m.voltage==3.895f&&m.has_battery_level&&m.battery_level==70);
  tick(9999);assert(mock.enable_calls==1&&mock.read_calls==8);tick(10000);assert(mock.enable_calls==2&&mock.refs==1);emptyCell(10000);tick(10009);assert(mock.read_calls==8);tick(10010);assert(mock.read_calls==16&&mock.sequence_calls==16&&mock.disable_calls==2&&mock.refs==0&&mock.setup_calls==1);
 }else if(name=="enable_error_clears_previous"){
  burst(0);assert(metrics(10).has_voltage);mock.enable_status=-EIO;mock.usb=true;tick(10000);emptyCell(10000);assert(metrics(10000).battery_level==101&&!mock.refs&&mock.enable_calls==2&&mock.disable_calls==1&&mock.read_calls==8);tick(10010);assert(mock.read_calls==8);mock.enable_status=0;mock.usb=false;burst(20000);assert(metrics(20010).voltage==3.895f&&metrics(20010).battery_level==70);
 }else if(name=="sequence_error"||name=="read_error"||name=="negative_sample"||name=="overflow_sample"){
  burst(0);assert(metrics(10).has_voltage);if(name=="sequence_error")mock.sequence_status[3]=-EIO;if(name=="read_error")mock.read_status[3]=-EIO;if(name=="negative_sample")mock.raw[3]=-1;if(name=="overflow_sample")mock.raw[3]=4096;
  tick(10000);emptyCell(10000);tick(10010);emptyCell(10010);assert(!metrics(10010).has_battery_level&&mock.sequence_calls==16&&mock.read_calls==(name=="sequence_error"?15u:16u)&&mock.disable_calls==2&&!mock.refs);
 }else if(name=="disable_error_stops_attempts"){
  burst(0);assert(metrics(10).has_voltage);mock.disable_status=-EIO;mock.usb=true;burst(10000);emptyCell(10010);assert(metrics(10010).battery_level==101&&mock.refs==1&&mock.enable_calls==2&&mock.disable_calls==2);tick(20000);tick(20010);assert(mock.enable_calls==2&&mock.read_calls==16&&mock.disable_calls==2);emptyCell(20010);
 }else if(name=="raw_and_envelope_boundaries"){
  const int16_t samples[]={0,4095,462,463,1039,1040};const bool valid[]={false,false,false,true,true,false};const uint16_t expected[]={0,0,0,2003,4496,0};
  for(unsigned j=0;j<6;++j){raw(samples[j]);burst(uint64_t(j)*10000);const auto s=mt::hardware::snapshot(uint64_t(j)*10000+10);assert(bool(s.cellMilliVolts)==valid[j]);if(valid[j])assert(*s.cellMilliVolts==expected[j]&&s.cellPercent&&*s.cellPercent==(j==3?0:100));else emptyCell(uint64_t(j)*10000+10);}
  assert(mock.sequence_calls==48&&mock.read_calls==48&&mock.enable_calls==6&&mock.disable_calls==6&&!mock.refs);
 }else if(name=="freshness_and_clock"){
  burst(100);assert(metrics(30110).has_voltage);emptyCell(30111);const auto stale=mt::hardware::snapshot(30111);assert(!stale.externalPower&&stale.capturedMs==0);emptyCell(109);assert(!mt::hardware::snapshot(109).externalPower);emptyCell(std::numeric_limits<uint64_t>::max());
 }else if(name=="usb_independent"){
  raw(0);mock.usb=true;burst(0);emptyCell(10);assert(metrics(10).has_battery_level&&metrics(10).battery_level==101);mock.usb=false;burst(10000);emptyCell(10010);assert(!metrics(10010).has_battery_level);raw(900);mock.usb=true;burst(20000);assert(metrics(20010).voltage==3.895f&&metrics(20010).battery_level==101);mock.usb=false;burst(30000);assert(metrics(30010).battery_level==70);
 }else if(name=="no_divider"){
#ifdef MT_MOCK_NO_DIVIDER
  burst(0);assert(mock.setup_calls==1&&mock.read_calls==8&&mock.sequence_calls==8&&!mock.enable_calls&&!mock.disable_calls&&!mock.refs&&metrics(10).voltage==3.895f);
#else
  assert(false);
#endif
 }else{assert(false);}
#endif
 assert(mock.violations==0);std::cout<<"PASS hardware/"<<name<<"\n";return 0;}
