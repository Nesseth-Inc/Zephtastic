// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ExtendedNotificationConfig.h"
#include "RingtoneSettings.h"
#include "NotificationController.h"
#include <cstddef>
#include <cstdint>
namespace mt::notification {
// Remaining stock outputs (vibrator, direct buzzer, PWM RTTTL, I2S RTTTL)
// layered beside the existing GPIO LED controller.
class ExtendedNotificationController {
public:
  bool configure(const ExtendedNotificationConfig&,ExtendedNotificationCapabilities,ExtendedNotificationBackend);
  bool setRingtone(const char*,size_t);
  bool receive(uint32_t from,uint32_t local,uint32_t id,uint32_t port,const uint8_t*,size_t,bool muted,bool rangeEnabled,uint32_t nowMs);
  bool startNotification(uint32_t nowMs);
  void poll(uint32_t nowMs);
  void stop();
  bool armed() const { return armed_; }
  bool available()const{return available_;}
private:
  ExtendedNotificationConfig config_{};ExtendedNotificationCapabilities caps_{};ExtendedNotificationBackend backend_{};
  bool available_=false,armed_=false,led_=false,vibrator_=false,buzzer_=false;
  bool eventLed_=false,eventVibrator_=false,eventBuzzer_=false,ringParsed_=false,notePending_=false;
  uint32_t cutoff_=0,lastToggle_=0;bool seen(uint32_t from,uint32_t id,uint32_t now)const;
  char ringtone_[RingtoneSettings::Capacity]="meshtastic:d=4,o=5,b=140:c";size_t ringtoneSize_=sizeof(RingtoneSettings::DefaultRingtone)-1;
  size_t ringtoneCursor_=0;uint32_t noteCutoff_=0;RingtoneParser::Settings ringSettings_{};
  struct Seen{uint32_t from=0,id=0,at=0;} seen_[4]{};unsigned next_=0;
  bool arm(bool led,bool vibra,bool buzzer,uint32_t now);
  void output(bool led,bool vibra,bool buzzer);bool ring(bool begin);
};
}
