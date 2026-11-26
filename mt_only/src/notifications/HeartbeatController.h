// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "HeartbeatPulse.h"
namespace mt {
// The main loop owns scheduling/GPIO. One transition per poll reproduces the
// donor's immediate start and rearm-from-now policy without a catch-up burst.
class HeartbeatController {
  bool enabled_=false,phaseOn_=false,light_=false;
  uint32_t due_=0;
public:
  void configure(bool enabled,uint32_t now){
    if(enabled==enabled_)return;
    enabled_=enabled;phaseOn_=light_=false;due_=now;
  }
  void poll(heartbeat::Mode mode,bool unread,uint32_t now){
    if(!enabled_||!heartbeat::deadlineReached(now,due_))return;
    const auto policy=heartbeat::plan(mode,unread);
    if(phaseOn_){light_=false;due_=heartbeat::rearmFrom(now,policy.nextAfterOffMs);}
    else{light_=policy.light;due_=heartbeat::rearmFrom(now,policy.pulseMs);}
    phaseOn_=!phaseOn_;
  }
  bool light()const{return enabled_&&light_;}
};
}
