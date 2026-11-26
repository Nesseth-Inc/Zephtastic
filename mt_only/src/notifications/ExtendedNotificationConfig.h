// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include "meshtastic/module_config.pb.h"
namespace mt::notification {
struct ExtendedNotificationConfig {
  bool enabled=false,active=false,usePwm=false,useI2s=false;
  bool alertMessage=false,alertBell=false,alertMessageVibra=false,alertBellVibra=false,alertMessageBuzzer=false,alertBellBuzzer=false;
  uint8_t led=0,vibrator=0,buzzer=0;uint32_t outputMs=1000,nagTimeoutSeconds=0;
  static bool fromWire(const meshtastic_ModuleConfig&,ExtendedNotificationConfig&);
};
struct ExtendedNotificationCapabilities { bool led=false,vibrator=false,gpioBuzzer=false,pwmBuzzer=false,i2sRtttl=false; };
struct ExtendedNotificationBackend {
  void *context=nullptr;
  bool (*setLed)(void*,bool)=nullptr;
  bool (*setVibrator)(void*,uint8_t,bool)=nullptr;
  bool (*setBuzzer)(void*,uint8_t,bool)=nullptr;
  bool (*setPwmTone)(void*,uint16_t,uint8_t)=nullptr;
  bool (*playRtttl)(void*,const char*,size_t,bool)=nullptr; // true starts/advances one bounded playback step
  void (*stopRtttl)(void*)=nullptr;
};
}
