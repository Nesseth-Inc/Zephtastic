// SPDX-License-Identifier: GPL-3.0-only
#include "power/Max17048.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <queue>
using std::isnan;
using std::clamp;
constexpr int MAX1704X_VCELL_REG=2, MAX1704X_SOC_REG=4, MAX1704X_CRATE_REG=0x16, MSBFIRST=1;
struct Registers {
  uint16_t values[32]{};
  unsigned reads=0;
  int fail=-1;
  bool set(uint8_t reg,uint16_t value){if(reg==fail)return false;values[reg]=value;return true;}
  bool word(uint8_t reg,uint16_t &out){++reads;out=values[reg];return reg!=fail;}
} regs;
struct Adafruit_BusIO_Register {
  int reg;
  Adafruit_BusIO_Register(Registers *,int address,int,int):reg(address){}
  uint16_t read(){return regs.values[reg];}
};
struct Adafruit_MAX17048 {
  Registers *i2c_dev=&regs;
  bool isDeviceReady(){return (regs.values[8]&0xfff0)==0x10;}
  float cellVoltage(),cellPercent(),chargeRate();
} gauge;
struct MAX17048Sensor {
  Adafruit_MAX17048 *max17048=&gauge;
  bool getMetrics(meshtastic_Telemetry *);
} donor;
constexpr int MAX17048_CHARGING_SAMPLES=3, MAGIC_USB_BATTERY_LEVEL=101;
constexpr float MAX17048_CHARGING_MINIMUM_RATE=1.0f, MAX17048_BUS_POWER_VOLTS=4.195f;
enum class MAX17048ChargeState{IDLE, EXPORT, IMPORT};
struct MAX17048ChargeSample{float cellPercent,chargeRate;};
struct MAX17048Singleton:Adafruit_MAX17048{
  std::queue<MAX17048ChargeSample> chargeSamples;
  MAX17048ChargeState chargeState{};
  bool isBatteryCharging(),isExternallyPowered(),isBatteryConnected();
  uint16_t getBusVoltageMv();uint8_t getBusBatteryPercent();
} chip;
struct Status{
  bool has=false,charging=false;
  int mv=-1;unsigned soc=0;
  void update(){has=chip.isBatteryConnected();charging=chip.isBatteryCharging();
    mv=has?chip.getBusVoltageMv():-1;soc=has?chip.getBusBatteryPercent():0;}
  bool getHasBattery(){return has;}bool getIsCharging(){return charging;}
  unsigned getBatteryChargePercent(){return soc;}int getBatteryVoltageMv(){return mv;}
} state;
static auto *powerStatus=&state;
struct DeviceTelemetryModule{meshtastic_Telemetry getDeviceTelemetry();};
struct Air{float utilizationTXPercent(){return 0;}float channelUtilizationPercent(){return 0;}} air;
static auto *airTime=&air;
static uint32_t getTime(){return 0;}
namespace Time {uint32_t getUptimeSecs(){return 0;}}
#define LOG_TRACE(...)
#define LOG_DEBUG(...)
#include "donor.inc"
static void trend(){
  mt::power::max17048::Sample sample;DeviceTelemetryModule device;
  unsigned step=0;regs.values[8]=0x0013;
  auto check=[&](uint16_t voltage,uint16_t soc,int16_t rate,bool charged){
    const uint64_t ms=step++*20000u;
    regs.values[2]=voltage;regs.values[4]=soc;regs.values[0x16]=uint16_t(rate);
    sample.poll(regs,ms);const auto reading=sample.get(ms);assert(sample.identified()&&reading.present);
    state.update();const auto stock=device.getDeviceTelemetry().variant.device_metrics;
    mt::HardwareSnapshot hardware;reading.device(hardware);meshtastic_DeviceMetrics zt{};hardware.apply(zt);
    assert(state.charging==charged&&reading.charging==charged);
    assert(zt.has_battery_level==stock.has_battery_level&&zt.battery_level==stock.battery_level);
    assert(zt.has_voltage==stock.has_voltage&&zt.voltage==stock.voltage);
    assert(chip.isExternallyPowered()==bool(hardware.externalPower&&*hardware.externalPower));
  };
  check(53760,50*256,5,false); // 4.20 V with stable SOC: external supply, no charging.
  check(53760,50*256,6,false);
  check(49920,50*256+1,5,true); // 3.9 V and sub-percent rise, even before rounded SOC changes.
  regs.values[4]=1;sample.poll(regs,41000);assert(sample.get(41000).charging);
  check(49920,50*256-1,5,false);
  check(49920,51*256,4,false); // 0.832 %/hr is below the donor threshold.
  check(49920,52*256,-36,false);
  check(49920,53*256,6,false); // Oldest endpoint had insufficient rate.
  check(49920,54*256,6,false);
  check(49920,55*256,6,true);
  regs.fail=0x16;sample.poll(regs,180000);assert(!sample.get(180000).present&&sample.identified());
  regs.fail=-1;sample.poll(regs,181000);assert(sample.get(181000).present&&!sample.get(181000).charging);
}
int main(){
  trend();
  regs.values[8]=0x0013;regs.values[MAX1704X_CRATE_REG]=2;
  unsigned n=0;
  for(unsigned raw:{25600u,33001u,45146u,51201u,53696u,57600u}){
    regs.values[2]=raw;
    for(unsigned soc:{0u,127u,128u,255u,256u,12611u,12799u,25471u,25472u,25600u,65535u}){
      ++n;regs.values[4]=soc;
      const auto reading=mt::power::max17048::measure(regs);
      assert(reading.present);
      meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_device_metrics_tag;
      assert(donor.getMetrics(&t)&&t.variant.device_metrics.battery_level==reading.percent&&t.variant.device_metrics.voltage==reading.volts);
      t={};t.which_variant=meshtastic_Telemetry_power_metrics_tag;
      assert(donor.getMetrics(&t));
      zt::telemetry::PowerReadings channels;channels.current[0]=10.0f;reading.power(channels);
      assert(channels.voltage[0]&&*channels.voltage[0]==t.variant.power_metrics.ch1_voltage&&*channels.current[0]==10.0f);
      mt::HardwareSnapshot hardware;reading.device(hardware);meshtastic_DeviceMetrics device{};hardware.apply(device);
      assert(device.has_voltage&&device.voltage==float(uint16_t(reading.volts*1000))/1000.0f);
      assert(device.has_battery_level&&device.battery_level==reading.percent);
    }
  }
  mt::power::max17048::Sample sample;regs.values[2]=44556;
  sample.poll(regs,100);const auto reads=regs.reads;assert(sample.get(100).present);
  sample.poll(regs,101);assert(regs.reads==reads);
  assert(!sample.get(99).present&&!sample.get(5100).present);
  for(int reg:{8,6,12,2,4}){
    regs.fail=reg;sample.poll(regs,2100);assert(!sample.get(2100).present);
    regs.fail=-1;sample.poll(regs,100);assert(sample.get(100).present);
  }
  regs.values[6]=0x2101;regs.values[12]=0x8bc1;
  assert(!mt::power::max17048::measure(regs).present&&regs.values[6]==0x0101&&regs.values[12]==0x8b41);
  assert(mt::power::max17048::measure(regs).present);
  regs.values[8]=0xbeef;assert(!mt::power::max17048::measure(regs).present);
  std::printf("PASS %u pinned MAX17048 device and power comparisons, rounding, charger threshold, read failures, stale samples and recovery\n",n);
}
