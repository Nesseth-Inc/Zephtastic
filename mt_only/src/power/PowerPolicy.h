// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include "meshtastic/config.pb.h"

namespace mt::power {
class PowerPolicy {
public:
  enum class State : uint8_t { Awake, LightSleepEligible, DeepSleepPending, BatteryShutdownPending, DeepSleepUnavailable, Unsupported };
  enum class DeepSleepResult : uint8_t { Resumed, TimerTick, NotEligible, NoConsent, NoWakePath, NotQuiescent, BackendFailed };
  struct Activity {
    bool application=false,radio=false,bluetooth=false,usb=false;
    // These are measured power-source observations, not USB transport state.
    bool externalPowerKnown=false,externalPower=false;
  };
  // Installed by the board/owner. Invoked only from executeDeepSleep(), which
  // the caller must run after releasing the service lock. A timed sleep returns
  // after timer/activity wake; a system-off backend normally never returns.
  struct DeepSleepBackend {
    void *context=nullptr;
    bool (*wakePathReady)(void *,uint32_t durationSeconds)=nullptr;
    bool (*prepare)(void *)=nullptr;
    bool (*quiescent)(void *)=nullptr;
    int (*enter)(void *,uint32_t durationSeconds)=nullptr;
    // Reopen the owner after a timed wake. System-off normally does not return.
    void (*resume)(void *)=nullptr;
  };
  struct PowerOffBackend {
    void *context=nullptr;
    bool (*wakePathReady)(void *)=nullptr;
    bool (*prepare)(void *)=nullptr;
    bool (*quiescent)(void *)=nullptr;
    int (*enter)(void *)=nullptr; // Must actually power off; a return is failure.
    void (*resume)(void *)=nullptr;
  };
private:
  meshtastic_Config_PowerConfig config_{};
  bool configured_=false,wasAwake_=true,pmLockHeld_=false;
  uint64_t lastActivityMs_=0,lastBleActivityMs_=0,armedAtMs_=0,lastPoweredMs_=0;
  uint64_t deepSleepDeadlineMs_=0;
  bool deepSleepScheduled_=false;
  uint64_t roleGraceUntil_=0;
  uint32_t observedRole_=UINT32_MAX,roleSleepSeconds_=0;
  State state_=State::Unsupported;
  DeepSleepBackend deepSleep_{};
  PowerOffBackend powerOff_{};
  void holdAwake();
  void allowIdle();
public:
  bool configure(const meshtastic_Config_PowerConfig &config,uint64_t nowMs);
  bool configSupported(const meshtastic_Config_PowerConfig &config)const;
  void setDeepSleepBackend(const DeepSleepBackend &backend){deepSleep_=backend;}
  void setPowerOffBackend(const PowerOffBackend &backend){powerOff_=backend;}
  bool supportsDeepSleep()const{return deepSleep_.wakePathReady&&deepSleep_.prepare&&deepSleep_.quiescent&&deepSleep_.enter&&deepSleep_.resume;}
  bool supportsPowerOff()const{return powerOff_.wakePathReady&&powerOff_.prepare&&powerOff_.quiescent&&powerOff_.enter&&powerOff_.resume;}
  // Effective override is consumed by the board battery-measurement owner.
  // Zero means retain the board's devicetree multiplier.
  bool powerMonitorEnabled()const{return configured_ && config_.powermon_enables!=0;}
  float adcMultiplierOverride()const{return configured_?config_.adc_multiplier_override:0.0f;}
  // Activity is sampled by the owner; busy/USB states immediately disarm sleep.
  void roleSleep(uint32_t role,uint32_t reportIntervalSeconds,uint64_t nowMs);
  State poll(uint64_t nowMs,const Activity &activity);
  // Separate execution phase: call outside Service/Companion locks after the
  // owner has stopped accepting work. No poweroff occurs from poll().
  DeepSleepResult executeDeepSleep(const Activity &activity);
  DeepSleepResult executeBatteryShutdown(const Activity &activity);
  // Explicit locally admitted shutdown request. It uses the separate
  // manual-wake power-off backend and never runs from poll()/an ISR.
  DeepSleepResult executeRequestedPowerOff(const Activity &activity,bool explicitConsent=false);
  bool supportsRequestedPowerOff()const{return supportsPowerOff()&&powerOff_.wakePathReady(powerOff_.context);}
  State state()const{return state_;}
  bool configured()const{return configured_;}
  bool platformReady()const;
  // True means the Zephyr kernel timed-idle path is available. CONFIG_PM adds
  // SoC policy control; the timed backend itself is the kernel semaphore/timer.
  bool armed()const{return pmLockHeld_==false&&state_==State::LightSleepEligible;}
  uint64_t lastActivityMs()const{return lastActivityMs_;}
};
} // namespace mt::power
