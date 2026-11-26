// SPDX-License-Identifier: GPL-3.0-only
// ADC/divider and VBUS acquisition adapted from ZephCore ZephyrBoard.cpp,
// liquidraver/ZephCore 3c7a7c830ac4cdbd0df052f06995d3c7632b9b44 (MIT).
// See zephcore/helpers/LICENSE.ZephCore. No MeshCore preferences or power policy.
#include "ZephyrHardware.h"
#include "power/InaBatterySelector.h"
#if defined(CONFIG_MT_MAX17048)
#include "power/Max17048.h"
#endif
#include "battery_curve.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#if defined(CONFIG_SOC_SERIES_NRF52)
#include <hal/nrf_power.h>
#elif defined(CONFIG_SOC_ESP32S3)
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(adc), okay) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#define MT_HAS_BATTERY_ADC 1
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/regulator.h>
static const adc_dt_spec batteryAdc=ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const device *const divider=DEVICE_DT_GET_OR_NULL(DT_NODELABEL(vbat_enable));
#endif
namespace mt::hardware {
bool siliconId(uint8_t (&out)[16]) {
#if defined(CONFIG_SOC_SERIES_NRF52)
  const uint32_t words[]={NRF_FICR->DEVICEID[0],NRF_FICR->DEVICEID[1],NRF_FICR->DEVICEADDR[0],NRF_FICR->DEVICEADDR[1]};
  memcpy(out,words,sizeof(words));return true;
#elif defined(CONFIG_SOC_ESP32S3)
  return esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID,out,128)==ESP_OK;
#else
  (void)out;return false;
#endif
}
static HardwareSnapshot current;
static uint64_t lastCycle=0,settleUntil=0;
static bool started=false;
static float adcMultiplierOverride=0.0f;
static uint32_t inaBatteryAddress=0;
static bool inaConfigurationReady=true;
#if defined(MT_HAS_BATTERY_ADC)
static bool adcReady=false,dividerHeld=false;
#endif
static zt::Observed<bool> externalPower(){
#if defined(CONFIG_SOC_SERIES_NRF52)
  return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#else
  return {};
#endif
}
void init(){
#if defined(MT_HAS_BATTERY_ADC)
  adcReady=adc_is_ready_dt(&batteryAdc)&&(!divider||device_is_ready(divider))&&
    adc_channel_setup_dt(&batteryAdc)==0;
#endif
}
void setAdcMultiplierOverride(float multiplier){
  // PowerPolicy validates the supported range against the board's declared
  // battery ADC. Retain a second local guard so an invalid value cannot
  // fabricate a plausible voltage if a caller bypasses that policy.
  adcMultiplierOverride=(multiplier>=2.0f&&multiplier<=6.0f)?multiplier:0.0f;
}
#if defined(CONFIG_MT_MAX17048)
static void cancelAdcBurst(){
  settleUntil=0;
#if defined(MT_HAS_BATTERY_ADC)
  if(dividerHeld){
    if(regulator_disable(divider)==0)dividerHeld=false;
    else adcReady=false;
  }
#endif
}
#endif
bool setInaBatteryAddress(uint32_t address){
  if(address==inaBatteryAddress)return inaConfigurationReady;
  // The sensor selector and the environmental adapter share this main-thread
  // owner. A ready node is already initialized; an unready selected INA may be
  // initialized by configure() once, and all subsequent fetches are serialized
  // with environment::poll by the application loop.
  inaBatteryAddress=address;
#if defined(CONFIG_MT_MAX17048)
  cancelAdcBurst();
  current.gaugeCharging={};
#elif defined(MT_HAS_BATTERY_ADC)
  if(settleUntil){
    const bool disabled=!dividerHeld||regulator_disable(divider)==0;
    dividerHeld=false;settleUntil=0;if(!disabled)adcReady=false;
  }
#else
  settleUntil=0;
#endif
  current.cellMilliVolts={};current.cellPercent={};
  inaConfigurationReady=power::ina::configure(address);
  return inaConfigurationReady;
}
void poll(uint64_t now){
#if defined(CONFIG_MT_MAX17048)
  power::max17048::poll(now);
  if (!inaBatteryAddress && power::max17048::identified()) {
    // A declared but absent accessory must leave the onboard ADC usable.
    // Once the gauge has answered, retry a temporarily unreadable gauge
    // without switching the identity of the battery measurement.
    cancelAdcBurst();
    current={};current.capturedMs=now;current.externalPower=externalPower();
    power::max17048::snapshot(now).device(current);
    return;
  }
#endif
  if(inaBatteryAddress){
#if defined(CONFIG_MT_MAX17048)
    cancelAdcBurst();
#endif
    // This selected source is authoritative. In particular, sensor errors,
    // stale data, or an unsupported DT address do not trigger ADC fallback.
    current.cellMilliVolts={};current.cellPercent={};
    current.externalPower=externalPower();current.capturedMs=now;
    power::ina::poll(now);
    const auto reading=power::ina::snapshot(now);
    if(reading.available()&&reading.busVoltageV>=2.0f&&reading.busVoltageV<=4.5f){
      const uint16_t millivolts=uint16_t(reading.busVoltageV*1000.0f);
      current.cellMilliVolts=millivolts;
      current.cellPercent=battery_curve_lookup(&battery_curve_default,millivolts);
    }
    return;
  }
  if(!settleUntil){
    if(started&&(now<lastCycle||now-lastCycle<10000))return;
    started=true;lastCycle=now;
    // Clear previous readings before every attempt, including enable failure.
    current={};current.capturedMs=now;current.externalPower=externalPower();
#if defined(MT_HAS_BATTERY_ADC)
    if(!adcReady)return;
    if(divider){
      if(regulator_enable(divider)!=0)return;
      dividerHeld=true;
    }
    settleUntil=now+10;return;
#else
    return;
#endif
  }
  if(now<settleUntil)return;
  settleUntil=0;
#if defined(MT_HAS_BATTERY_ADC)
  int32_t sum=0;unsigned count=0;
  for(unsigned i=0;i<8;++i){
    int16_t raw=0;adc_sequence seq{};seq.buffer=&raw;seq.buffer_size=sizeof(raw);
    if(adc_sequence_init_dt(&batteryAdc,&seq)==0&&adc_read_dt(&batteryAdc,&seq)==0&&raw>=0&&raw<=4095){
      sum+=raw;++count;
    }
  }
  // Drop our regulator reference on every completed burst. On disable failure
  // stop further ADC attempts; never accumulate enable references or stale data.
  const bool disabled=!dividerHeld||regulator_disable(divider)==0;
  dividerHeld=false;if(!disabled)adcReady=false;
  current.capturedMs=now;current.externalPower=externalPower();
  if(!disabled||count!=8)return;
  float multiplier=float(DT_PROP(DT_PATH(zephyr_user),vbat_mv_multiplier));
  if(adcMultiplierOverride>0.0f){
    // The stock preference is a divider ratio. The board default already
    // includes the ADC reference and gain in its full-scale millivolts.
    int32_t fullScaleMv=4096;
    if(adc_raw_to_millivolts_dt(&batteryAdc,&fullScaleMv)||fullScaleMv<=0)return;
    multiplier=adcMultiplierOverride*float(fullScaleMv);
  }
  const uint32_t mv=uint32_t((float(sum)/float(count))*multiplier/4096.0f);
  // Single-cell operating envelope, not a battery-presence detector. Below
  // range, a floating/no-cell ADC and a depleted cell cannot be distinguished.
  if(mv<2000||mv>4500)return;
  current.cellMilliVolts=uint16_t(mv);
  current.cellPercent=battery_curve_lookup(&battery_curve_default,uint16_t(mv));
#endif
}
HardwareSnapshot snapshot(uint64_t now){return current.fresh(now);}
}
