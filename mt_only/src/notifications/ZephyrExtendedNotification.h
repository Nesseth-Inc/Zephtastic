// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ExtendedNotificationConfig.h"
#include "RingtoneParser.h"

namespace mt::audio { class ZephyrI2sAudio; }
namespace mt::notification {
// DT-owned notification peripherals. Only explicitly declared aliases are
// driven; generated pin numbers are accepted only when they match that alias.
class ZephyrExtendedNotification {
public:
  ZephyrExtendedNotification()=default;
  ~ZephyrExtendedNotification();
  static ExtendedNotificationCapabilities availableCapabilities(const meshtastic_ModuleConfig&,const ExtendedNotificationCapabilities &allowed);
  static ExtendedNotificationCapabilities availableCapabilities(const ExtendedNotificationConfig&,const ExtendedNotificationCapabilities &allowed);
  ExtendedNotificationCapabilities configure(const ExtendedNotificationConfig&,audio::ZephyrI2sAudio *audio=nullptr);
  ExtendedNotificationCapabilities configure(const meshtastic_ModuleConfig&,const ExtendedNotificationCapabilities &allowed,audio::ZephyrI2sAudio *audio=nullptr);
  ExtendedNotificationCapabilities capabilities()const{return caps_;}
  ExtendedNotificationBackend backend();
  void stop();
private:
  ExtendedNotificationConfig config_{};ExtendedNotificationCapabilities caps_{};audio::ZephyrI2sAudio *audio_=nullptr;
  bool vibratorOn_=false,buzzerOn_=false,rtttlActive_=false;uint32_t nextToneAt_=0;size_t rtttlCursor_=0,rtttlSize_=0;
  const char *rtttlText_=nullptr;RingtoneParser::Settings rtttlSettings_{};
  bool setLed(bool);bool setVibrator(uint8_t,bool);bool setBuzzer(uint8_t,bool);bool setPwmTone(uint16_t,uint8_t);
  ExtendedNotificationCapabilities configureInternal(const ExtendedNotificationConfig&,const ExtendedNotificationCapabilities&,audio::ZephyrI2sAudio*);
  bool playRtttl(const char*,size_t,bool);void stopRtttl();
  static bool ledThunk(void*,bool);static bool vibraThunk(void*,uint8_t,bool);static bool buzzerThunk(void*,uint8_t,bool);
  static bool pwmThunk(void*,uint16_t,uint8_t);static bool rtttlThunk(void*,const char*,size_t,bool);static void stopThunk(void*);
};
}
