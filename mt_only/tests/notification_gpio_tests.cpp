// SPDX-License-Identifier: GPL-3.0-only
// Synthetic Zephyr GPIO/device functions, real driver and real C LED gate.
// Single-thread fixture atomics are not evidence of concurrency correctness.
#include "notifications/ZephyrNotification.h"
#include "led_gate.h"
#include <zephyr/drivers/gpio.h>
#include <cassert>
#include <cstdio>
#include <cerrno>
#include <initializer_list>
namespace {
bool deviceReady=true,commitOnError=false;int configureResult=0,writeResult=0;
unsigned readyCalls=0,configureCalls=0,writeCalls=0,uiCalls=0;bool uiDisabled=false;
int physical=-1;uint32_t configuredFlags=0,lastPin=0;
unsigned groups=0;
void pass(const char *text){++groups;std::printf("PASS %s\n",text);}
void gates(){zephcore_leds_set_disabled(false);zephcore_leds_set_radio_mode(LEDS_RADIO_TX);zephcore_leds_set_hb_mode(LEDS_HB_ALL);mt::notification::radioActive(false);}
void reset(){gates();deviceReady=true;commitOnError=false;configureResult=writeResult=0;readyCalls=configureCalls=writeCalls=0;physical=-1;configuredFlags=lastPin=0;}
void start(){reset();mt::notification::init();}
}
extern "C" {
struct device fixture_gpio_device={1};
void zephcore_leds_ui_sync(bool disabled){++uiCalls;uiDisabled=disabled;}
bool fixture_gpio_ready(const gpio_dt_spec *spec){assert(spec&&spec->port==&fixture_gpio_device);++readyCalls;lastPin=spec->pin;return deviceReady;}
int fixture_gpio_configure(const gpio_dt_spec *spec,uint32_t flags){assert(spec&&spec->port==&fixture_gpio_device);++configureCalls;lastPin=spec->pin;configuredFlags=flags;if(!configureResult)physical=0;return configureResult;}
int fixture_gpio_set(const gpio_dt_spec *spec,int value){assert(spec&&spec->port==&fixture_gpio_device);assert(value==0||value==1);++writeCalls;lastPin=spec->pin;if(!writeResult||commitOnError)physical=value;return writeResult;}
}
int main(){
  assert(!mt::notification::available());
  assert(!zephcore_leds_disabled()&&zephcore_leds_radio_mode()==LEDS_RADIO_TX&&zephcore_leds_hb_mode()==LEDS_HB_ALL&&!zephcore_led_radio_holds_pin());
  mt::notification::render({true,true,true});assert(!readyCalls&&!configureCalls&&!writeCalls&&physical==-1);
  pass("cold driver refuses GPIO before init; literal C donor defaults are enabled/TX/all with no hold");
#if defined(TEST_NOTIFICATION_HEADLESS)
  start();assert(!mt::notification::available()&&mt::notification::pin()==0);
  for(bool disabled:{false,true})for(bool held:{false,true})for(unsigned mode:{0u,1u,2u,3u})for(bool enabled:{false,true})for(bool active:{false,true})for(bool on:{false,true}){
    zephcore_leds_set_disabled(disabled);mt::notification::radioActive(held);zephcore_leds_set_radio_mode(uint8_t(mode));mt::notification::render({enabled,active,on});
  }
  assert(!readyCalls&&!configureCalls&&!writeCalls&&physical==-1&&!mt::notification::available());
  mt::notification::render({},true,1);assert(!writeCalls&&physical==-1);
  pass("actual headless branch reports no capability/pin and never touches GPIO across all output and donor gate combinations");
#else
  start();assert(mt::notification::available()&&mt::notification::pin()==28&&readyCalls==1&&configureCalls==1&&!writeCalls&&lastPin==28&&physical==0&&configuredFlags==GPIO_OUTPUT_INACTIVE);
  mt::notification::render({});assert(!writeCalls&&physical==0);
  pass("T096 alias pin28 active-high configures known inactive exactly once before advertising readiness");
  for(bool enabled:{false,true})for(bool active:{false,true})for(bool on:{false,true}){
    start();mt::notification::render({enabled,active,on});const bool level=enabled&&(active?on:!on);
    assert(physical==int(level)&&writeCalls==unsigned(level));const auto count=writeCalls;mt::notification::render({enabled,active,on});assert(writeCalls==count);
  }
  pass("all enabled/active-high/active-low/on truth-table combinations and same-level no-ops use the real driver");
  for(bool active:{false,true})for(bool on:{false,true})for(unsigned mode:{0u,1u,2u,3u}){
    start();mt::notification::radioActive(true);zephcore_leds_set_radio_mode(uint8_t(mode));zephcore_leds_set_disabled(true);mt::notification::render({true,active,on});assert(physical==0&&!writeCalls);
  }
  start();mt::notification::render({true,true,true});assert(physical==1);const auto syncs=uiCalls;
  zephcore_leds_set_disabled(true);assert(uiCalls==syncs+1&&uiDisabled);mt::notification::render({true,true,true});assert(physical==0&&writeCalls==2);
  pass("master disable dominates notification polarity and every radio mode, including clearing a previously lit level");
  for(unsigned mode:{unsigned(LEDS_RADIO_TX),unsigned(LEDS_RADIO_RX),unsigned(LEDS_RADIO_ALL)}){
    start();zephcore_leds_set_radio_mode(uint8_t(mode));mt::notification::radioActive(true);mt::notification::render({false,false,false});assert(physical==1&&writeCalls==1);
    mt::notification::render({true,true,false});assert(physical==1&&writeCalls==1);mt::notification::radioActive(false);mt::notification::render({true,true,false});assert(physical==0&&writeCalls==2);
  }
  for(bool active:{false,true})for(bool on:{false,true}){
    start();mt::notification::radioActive(true);zephcore_leds_set_radio_mode(LEDS_RADIO_OFF);mt::notification::render({true,active,on});assert(physical==int(active?on:!on));
  }
  pass("radio hold wins in TX/RX/ALL, release restores notification level, and radio OFF yields without stealing the pin");
  for(auto mode:{LEDS_RADIO_TX,LEDS_RADIO_RX,LEDS_RADIO_ALL,LEDS_RADIO_OFF}){
    start();zephcore_leds_set_radio_mode(mode);
    const uint32_t t=UINT32_MAX-30;
    mt::notification::radioReceived(t);mt::notification::render({},false,t);
    assert(physical==int(mode==LEDS_RADIO_RX||mode==LEDS_RADIO_ALL));
    mt::notification::radioReceived(t+60);mt::notification::render({},false,t+159);
    assert(physical==int(mode==LEDS_RADIO_RX||mode==LEDS_RADIO_ALL));
    mt::notification::render({},false,t+160);assert(!physical);
  }
  start();mt::notification::render({},true,100);assert(physical==1);
  mt::notification::render({},true,119);assert(physical==1);
  mt::notification::render({},true,120);assert(physical==0);
  mt::notification::render({},true,4099);assert(physical==0);
  mt::notification::render({},true,4100);assert(physical==1);
  mt::notification::render({true,true,false},true,4101);assert(physical==0);
  mt::notification::radioActive(true);mt::notification::render({true,true,false},true,4102);assert(physical==1);
  zephcore_leds_set_disabled(true);mt::notification::render({},true,4103);assert(physical==0);
  mt::notification::radioActive(false);zephcore_leds_set_disabled(false);
  mt::notification::render({},true,4104);assert(physical==1);
  mt::notification::render({},false,4105);assert(physical==0);
  zephcore_leds_set_hb_mode(LEDS_HB_UNREAD);mt::notification::render({},true,4106);assert(physical==0);
  pass("heartbeat runs donor cycle behind notification/radio/master gates; disabling clears its pulse and no unread state is invented");
  for(bool committed:{false,true}){
    start();writeResult=-EIO;commitOnError=committed;mt::notification::render({true,true,true});
    assert(!mt::notification::available()&&writeCalls==1&&physical==int(committed));const auto writes=writeCalls;
    for(unsigned i=0;i<4;++i){mt::notification::radioActive(i&1);zephcore_leds_set_disabled(i&1);mt::notification::render({true,false,bool(i&1)});}
    assert(!mt::notification::available()&&writeCalls==writes);
  }
  pass("GPIO write failure freezes readiness and all later renders; physical outcome may be old or committed, never falsely qualified inactive");
  reset();deviceReady=false;mt::notification::init();assert(!mt::notification::available()&&readyCalls==1&&!configureCalls);mt::notification::render({true,true,true});assert(!writeCalls&&physical==-1);
  reset();configureResult=-EIO;mt::notification::init();assert(!mt::notification::available()&&configureCalls==1&&physical==-1);mt::notification::render({true,true,true});assert(!writeCalls);
  pass("unready device and failed output configuration never advertise capability or attempt writes");
#endif
  const auto previousWrites=writeCalls;const auto previousConfigs=configureCalls;
  for(unsigned mode:{0u,1u,2u,3u}){zephcore_leds_set_radio_mode(uint8_t(mode));zephcore_leds_set_hb_mode(uint8_t(3-mode));assert(zephcore_leds_radio_mode()==mode&&zephcore_leds_hb_mode()==3-mode);}
  mt::notification::radioActive(true);assert(zephcore_led_radio_holds_pin());mt::notification::radioActive(false);assert(!zephcore_led_radio_holds_pin());
  const auto finalSyncs=uiCalls;zephcore_leds_set_disabled(true);assert(zephcore_leds_disabled()&&uiCalls==finalSyncs+1&&uiDisabled);zephcore_leds_set_disabled(false);assert(!zephcore_leds_disabled()&&!uiDisabled);
  assert(writeCalls==previousWrites&&configureCalls==previousConfigs);
  pass("linked literal MIT donor atomic-gate APIs and weak UI override operate without direct GPIO; atomics are single-thread mocks only");
  std::printf("NOTIFICATION_GPIO_GROUPS %u\n",groups);
}
