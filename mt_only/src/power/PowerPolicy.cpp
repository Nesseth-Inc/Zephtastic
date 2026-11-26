// SPDX-License-Identifier: GPL-3.0-only
#include "PowerPolicy.h"
#include "InaBatterySelector.h"
#include "../DeviceRolePolicy.h"
#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#if defined(CONFIG_PM)
#include "../../../zephcore/helpers/pm_sleep_guard.h"
#endif

namespace mt::power {
namespace {
uint64_t secondsMs(uint32_t s){return uint64_t(s)*1000u;}
uint32_t waitBluetoothSecs(const meshtastic_Config_PowerConfig &c){return c.wait_bluetooth_secs?c.wait_bluetooth_secs:60u;}
uint32_t minWakeSecs(const meshtastic_Config_PowerConfig &c){return c.min_wake_secs?c.min_wake_secs:10u;}
uint32_t lightSleepSecs(const meshtastic_Config_PowerConfig &c){return c.ls_secs?c.ls_secs:300u;}
uint32_t deepSleepSecs(const meshtastic_Config_PowerConfig &c){return c.sds_secs?c.sds_secs:31536000u;}
uint32_t batteryShutdownSecs(const meshtastic_Config_PowerConfig &c){return c.on_battery_shutdown_after_secs<30u?30u:c.on_battery_shutdown_after_secs;}
bool same(const meshtastic_Config_PowerConfig &a,const meshtastic_Config_PowerConfig &b){
  return a.is_power_saving==b.is_power_saving&&
    a.on_battery_shutdown_after_secs==b.on_battery_shutdown_after_secs&&
    a.adc_multiplier_override==b.adc_multiplier_override&&
    a.wait_bluetooth_secs==b.wait_bluetooth_secs&&a.sds_secs==b.sds_secs&&
    a.ls_secs==b.ls_secs&&a.min_wake_secs==b.min_wake_secs&&
    a.device_battery_ina_address==b.device_battery_ina_address&&
    a.powermon_enables==b.powermon_enables;
}
bool batteryAdcDeclared(){
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user),io_channels) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user),vbat_mv_multiplier)
  return true;
#else
  return false;
#endif
}
bool batteryShutdownPowerSourceMeasured(){
  // This runtime has a measured VBUS/external-power signal on T096. The XIAO
  // ESP32-S3 composition currently has no declared external-power input, so
  // manual button shutdown may be supported there but battery-triggered
  // shutdown must remain unavailable.
#if defined(CONFIG_BOARD_HELTEC_T096) && defined(CONFIG_SOC_SERIES_NRF52)
  return true;
#else
  return false;
#endif
}
}
void PowerPolicy::holdAwake(){
#if defined(CONFIG_PM)
  if(!pmLockHeld_){zc_pm_block_sleep();pmLockHeld_=true;}
#endif
}
void PowerPolicy::allowIdle(){
#if defined(CONFIG_PM)
  if(pmLockHeld_){zc_pm_unblock_sleep();pmLockHeld_=false;}
#endif
}
bool PowerPolicy::platformReady()const{
  // This target always runs the Zephyr kernel. The owner-thread timed wait
  // uses k_sem/k_timeout even when optional system PM policy is disabled.
  // CONFIG_PM only adds the standby policy lock around ordinary idle periods.
  return true;
}
bool PowerPolicy::configSupported(const meshtastic_Config_PowerConfig &config)const{
  if(!(config.adc_multiplier_override==0.0f||
       (batteryAdcDeclared()&&config.adc_multiplier_override>=2.0f&&config.adc_multiplier_override<=6.0f)))return false;
  // These actions require a battery power source/ADC or diagnostic framework
  // that this policy does not own. Preserve them in settings, but do not claim
  // the policy applies them.
  if(config.on_battery_shutdown_after_secs&&config.on_battery_shutdown_after_secs!=UINT32_MAX&&
     (!batteryShutdownPowerSourceMeasured()||!supportsPowerOff()||
      !powerOff_.wakePathReady(powerOff_.context)))return false;
  // Only a declared, unique INA node with this exact devicetree reg address is
  // selectable. PowerMon has no source registry in this target composition.
  if(!ina::supports(config.device_battery_ina_address))return false;
#if !defined(CONFIG_MT_POWER_STRESS)
  if(config.powermon_enables)return false;
#endif
  if(config.is_power_saving&&(!platformReady()||!supportsDeepSleep()||
     !deepSleep_.wakePathReady(deepSleep_.context,deepSleepSecs(config))))return false;
  return true;
}
bool PowerPolicy::configure(const meshtastic_Config_PowerConfig &config,uint64_t now){
  if(now>INT64_MAX||!configSupported(config))return false;
  if(configured_&&same(config_,config))return true;
  config_=config;configured_=true;lastActivityMs_=now;lastBleActivityMs_=now;armedAtMs_=0;
  lastPoweredMs_=now;deepSleepDeadlineMs_=0;deepSleepScheduled_=false;roleSleepSeconds_=0;
  wasAwake_=true;state_=State::Awake;holdAwake();return true;
}
// Position and telemetry workers give queued RF five seconds before sleeping.
// The shared owner keeps the earliest pending report cycle and rechecks wake
// sources, queues and live consent before each bounded timer slice.
void PowerPolicy::roleSleep(uint32_t role,uint32_t seconds,uint64_t now){
  if(role!=observedRole_){
    if(roleSleepSeconds_){roleSleepSeconds_=0;deepSleepScheduled_=false;wasAwake_=true;lastActivityMs_=now;}
    observedRole_=role;
  }
  if(!configured_||!config_.is_power_saving||!seconds||now>INT64_MAX||
     (role!=DeviceRolePolicy::Role::SENSOR&&role!=DeviceRolePolicy::Role::TRACKER&&role!=10))return;
  if(roleSleepSeconds_){
    if(!deepSleepScheduled_&&seconds<roleSleepSeconds_)roleSleepSeconds_=seconds;
    return;
  }
  deepSleepScheduled_=false;roleSleepSeconds_=seconds;roleGraceUntil_=now+5000;
}
PowerPolicy::State PowerPolicy::poll(uint64_t now,const Activity &a){
  if(!configured_||now>INT64_MAX){holdAwake();state_=State::Unsupported;return state_;}
  if(a.externalPowerKnown&&a.externalPower)lastPoweredMs_=now;
  if(config_.on_battery_shutdown_after_secs&&config_.on_battery_shutdown_after_secs!=UINT32_MAX&&
     a.externalPowerKnown&&!a.externalPower&&now>=lastPoweredMs_&&
     now-lastPoweredMs_>=secondsMs(batteryShutdownSecs(config_))){
    holdAwake();state_=supportsPowerOff()?State::BatteryShutdownPending:State::Unsupported;return state_;
  }
  if(roleSleepSeconds_){
    if(a.usb||a.bluetooth||(now<roleGraceUntil_&&roleGraceUntil_-now>5000)||
       (deepSleepScheduled_&&(a.application||a.radio))||
       (now>=roleGraceUntil_&&(a.application||a.radio)&&now-roleGraceUntil_>=30000)){
      roleSleepSeconds_=0;deepSleepScheduled_=false;
    }else if(now>=roleGraceUntil_&&!a.application&&!a.radio){
      if(!deepSleepScheduled_){
        deepSleepDeadlineMs_=now+secondsMs(roleSleepSeconds_);deepSleepScheduled_=true;
      }
      allowIdle();state_=supportsDeepSleep()?State::DeepSleepPending:State::DeepSleepUnavailable;
      return state_;
    }else{holdAwake();state_=State::Awake;return state_;}
  }
  if(a.application||a.radio||a.usb){lastActivityMs_=now;wasAwake_=true;armedAtMs_=0;deepSleepScheduled_=false;holdAwake();state_=State::Awake;return state_;}
  if(a.bluetooth){lastBleActivityMs_=now;lastActivityMs_=now;wasAwake_=true;armedAtMs_=0;deepSleepScheduled_=false;holdAwake();state_=State::Awake;return state_;}
  if(!config_.is_power_saving){holdAwake();state_=State::Awake;return state_;}
  // Sleepy reporting roles stay available for their first fix/measurement.
  // Their module owns the sleep request and interval.
  if(observedRole_==DeviceRolePolicy::Role::SENSOR||observedRole_==DeviceRolePolicy::Role::TRACKER||observedRole_==10){
    allowIdle();state_=State::LightSleepEligible;return state_;
  }
  // Both the post-BLE grace and post-radio/application minimum-wake timer are
  // honored before Zephyr may select a low-power state.
  const uint64_t elapsed=now>=lastActivityMs_?now-lastActivityMs_:0;
  const uint64_t bleElapsed=now>=lastBleActivityMs_?now-lastBleActivityMs_:0;
  if(bleElapsed<secondsMs(waitBluetoothSecs(config_))||
     elapsed<secondsMs(minWakeSecs(config_))){
    holdAwake();state_=State::Awake;return state_;
  }
  if(wasAwake_){wasAwake_=false;armedAtMs_=now;}
  const uint64_t lightSleepElapsed=now>=armedAtMs_?now-armedAtMs_:0;
  if(lightSleepElapsed>=secondsMs(lightSleepSecs(config_))){
    // A deep-sleep path is pending only with an explicitly installed board
    // backend. No callback or poweroff is run in the owner polling context.
    if(supportsDeepSleep()){
      if(!deepSleepScheduled_){
        const uint32_t seconds=deepSleepSecs(config_);
        deepSleepDeadlineMs_=seconds==UINT32_MAX?UINT64_MAX:
          (now>UINT64_MAX-secondsMs(seconds)?UINT64_MAX:now+secondsMs(seconds));
        deepSleepScheduled_=true;
      }
      state_=State::DeepSleepPending;
    }
    else {holdAwake();state_=State::DeepSleepUnavailable;}
    return state_;
  }
  allowIdle();state_=State::LightSleepEligible;
  return state_;
}
PowerPolicy::DeepSleepResult PowerPolicy::executeDeepSleep(const Activity &a){
  if(state_!=State::DeepSleepPending)return DeepSleepResult::NotEligible;
  if(!config_.is_power_saving)return DeepSleepResult::NoConsent;
  if(a.application||a.radio||a.bluetooth||a.usb)return DeepSleepResult::NotQuiescent;
  if(!supportsDeepSleep())return DeepSleepResult::NoWakePath;
  const uint32_t duration=roleSleepSeconds_?roleSleepSeconds_:deepSleepSecs(config_);
  if(!deepSleep_.wakePathReady(deepSleep_.context,duration))return DeepSleepResult::NoWakePath;
  if(!deepSleep_.prepare(deepSleep_.context)){deepSleep_.resume(deepSleep_.context);return DeepSleepResult::NotQuiescent;}
  if(!deepSleep_.quiescent(deepSleep_.context)){deepSleep_.resume(deepSleep_.context);return DeepSleepResult::NotQuiescent;}
  // Recheck wake readiness after platform preparation, which may alter devices.
  if(!deepSleep_.wakePathReady(deepSleep_.context,duration)){deepSleep_.resume(deepSleep_.context);return DeepSleepResult::NoWakePath;}
  const int result=deepSleep_.enter(deepSleep_.context,duration);
  deepSleep_.resume(deepSleep_.context);
  if(result==1){
    // A timed maintenance wake is not activity. Keep the quiet timers and
    // pending low-power state intact so the next owner iteration can service
    // scheduled work/watchdog and then continue sleeping if still eligible.
    const int64_t wakeMs=k_uptime_get();
    if(wakeMs>=0&&deepSleepDeadlineMs_!=UINT64_MAX&&uint64_t(wakeMs)>=deepSleepDeadlineMs_){
      roleSleepSeconds_=0;deepSleepScheduled_=false;state_=State::Awake;wasAwake_=true;armedAtMs_=0;holdAwake();
    }else {allowIdle();state_=State::DeepSleepPending;}
    return DeepSleepResult::TimerTick;
  }
  if(result==0){roleSleepSeconds_=0;deepSleepScheduled_=false;state_=State::Awake;wasAwake_=true;armedAtMs_=0;lastActivityMs_=k_uptime_get();holdAwake();return DeepSleepResult::Resumed;}
  roleSleepSeconds_=0;deepSleepScheduled_=false;holdAwake();state_=State::DeepSleepUnavailable;return DeepSleepResult::BackendFailed;
}
PowerPolicy::DeepSleepResult PowerPolicy::executeBatteryShutdown(const Activity &a){
  if(state_!=State::BatteryShutdownPending)return DeepSleepResult::NotEligible;
  if(!a.externalPowerKnown||a.externalPower||a.application||a.radio||a.bluetooth||a.usb)return DeepSleepResult::NotQuiescent;
  if(!supportsPowerOff()||!powerOff_.wakePathReady(powerOff_.context))return DeepSleepResult::NoWakePath;
  if(!powerOff_.prepare(powerOff_.context)){powerOff_.resume(powerOff_.context);return DeepSleepResult::NotQuiescent;}
  if(!powerOff_.quiescent(powerOff_.context)){powerOff_.resume(powerOff_.context);return DeepSleepResult::NotQuiescent;}
  if(!powerOff_.wakePathReady(powerOff_.context)){powerOff_.resume(powerOff_.context);return DeepSleepResult::NoWakePath;}
  // A poweroff backend is required not to return. Any return means the actual
  // hardware action failed; resume the application so it can report/recover.
  (void)powerOff_.enter(powerOff_.context);powerOff_.resume(powerOff_.context);
  holdAwake();state_=State::Unsupported;return DeepSleepResult::BackendFailed;
}
PowerPolicy::DeepSleepResult PowerPolicy::executeRequestedPowerOff(const Activity &a,bool explicitConsent){
  if(!supportsRequestedPowerOff())return DeepSleepResult::NoWakePath;
  // Explicit local Admin consent may shut down while its transport remains
  // connected, but never overrides application/radio work. Automatic callers
  // retain the usual USB/BLE awake policy.
  if(a.application||a.radio||(!explicitConsent&&(a.bluetooth||a.usb)))return DeepSleepResult::NotQuiescent;
  if(!powerOff_.prepare(powerOff_.context)){powerOff_.resume(powerOff_.context);return DeepSleepResult::NotQuiescent;}
  if(!powerOff_.quiescent(powerOff_.context)){powerOff_.resume(powerOff_.context);return DeepSleepResult::NotQuiescent;}
  if(!powerOff_.wakePathReady(powerOff_.context)){powerOff_.resume(powerOff_.context);return DeepSleepResult::NoWakePath;}
  (void)powerOff_.enter(powerOff_.context);powerOff_.resume(powerOff_.context);
  holdAwake();state_=State::Unsupported;return DeepSleepResult::BackendFailed;
}
}
