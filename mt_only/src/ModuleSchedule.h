// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RelayCore.h"
namespace mt {
// Qwen recurring-deadline proposal, using the existing wrap-safe primitive.
class ModuleSchedule {
  uint32_t interval_=0,deadline_=0;bool enabled_=false;
public:
  void configure(uint32_t now,uint32_t interval,uint32_t firstDelay){
    if(!interval||interval>=0x80000000u||firstDelay>=0x80000000u){disable();return;}
    interval_=interval;deadline_=now+firstDelay;enabled_=true;
  }
  bool due(uint32_t now)const{return enabled_&&bench::due(now,deadline_);}
  void consumed(uint32_t now){if(enabled_)deadline_=now+interval_;}
  void disable(){enabled_=false;}
};
}
