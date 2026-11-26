// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NotificationSettings.h"
#include "../stock/peripherals/ExternalNotificationModule.h"
#include "meshtastic/portnums.pb.h"

namespace mt {
// RAM-only generic LED subset of pinned stock ExternalNotificationModule.
// Admission/decryption/replay/ignored-node filtering precede this callback.
class NotificationController {
  NotificationSettings settings_;
  bool available_=false,armed_=false,on_=false;
  uint32_t cutoff_=0,toggledAt_=0,interval_=0;
  struct Seen{uint32_t from=0,id=0,at=0;} seen_[4];unsigned next_=0;
  bool arm(uint32_t now){
    cutoff_ = stock::notification::armNagCycle(now, settings_.nagTimeoutSeconds(), settings_.outputMs());
    interval_=settings_.outputMs()?settings_.outputMs():1000;
    toggledAt_=now;armed_=true;on_=true;return true;
  }
public:
  struct Output{bool enabled=false,active=false,on=false;};
  void stop(){armed_=false;on_=false;}
  void configure(const NotificationSettings &s,bool available){
    const bool valid=available&&s.valid();
    // Record version, heartbeat and CRC changes do not alter this controller's
    // notification policy. Preserve an active pulse/nag across those edits.
    const bool samePolicy=!((settings_.bytes[4]^s.bytes[4])&0x0f)&&
      !memcmp(settings_.bytes+5,s.bytes+5,9);
    if(!samePolicy||available_!=valid)stop();
    settings_=s;available_=valid;
    if(!available_||!s.enabled())stop();
  }
  bool receive(uint32_t from,uint32_t local,uint32_t id,uint32_t port,const uint8_t *payload,size_t size,
               bool muted,bool rangeEnabled,uint32_t now){
    if(!available_||!settings_.enabled()||!id||from==local||!payload||size>233)return false;
    if (!stock::notification::wantPacket(port, rangeEnabled)) return false;
    for(const auto &s:seen_)if(s.from==from&&s.id==id&&uint32_t(now-s.at)<600000)return false;
    // Record the common USB/BLE fanout even when policy suppresses output.
    seen_[next_++%4]={from,id,now};
    const stock::notification::Flags flags{settings_.alertBell(), settings_.alertMessage(), false, false, false, false};
    if (!stock::notification::handleReceived(flags, payload, size, muted, false, false).generic) return false;
    return arm(now);
  }
  bool startNotification(uint32_t now){
    return available_ && settings_.enabled() && settings_.alertMessage() && arm(now);
  }
  void poll(uint32_t now){
    if(!armed_)return;
    if(stock::notification::deadlinePassed(now,cutoff_)){stop();return;}
    if(stock::notification::outputDue(now,toggledAt_,interval_)){on_=!on_;toggledAt_=now;}
  }
  Output output()const{return {available_&&settings_.enabled(),settings_.active(),on_};}
  bool armed()const{return armed_;}
};
}
