// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NodeCache.h"
namespace mt {
// Bounded admitted RF source observations, not NodeDB total or global mesh size.
// Local monotonic64 time alone determines age; no UTC, key or durable state.
class OnlineObservations {
  // Separate naturally aligned arrays avoid four padding bytes per observation.
  uint64_t seenAt_[64]{};
  uint32_t ids_[64]{};uint64_t highWater_=0;bool haveClock_=false;
public:
  static constexpr uint64_t Lifetime=7200000;
  bool advance(uint64_t now){
    if(now==UINT64_MAX||(haveClock_&&now<highWater_))return false;
    highWater_=now;haveClock_=true;return true;
  }
  bool observe(uint32_t id,uint32_t self,uint64_t now){
    if(id<=1||id>=0xfffffffeu||id==self||!advance(now))return false;
    unsigned slot=0;
    while(slot<64&&ids_[slot]!=id)++slot;
    if(slot==64)for(slot=0;slot<64&&ids_[slot]&&now-seenAt_[slot]<Lifetime;++slot){}
    if(slot==64){slot=0;for(unsigned i=1;i<64;i++)if(seenAt_[i]<seenAt_[slot])slot=i;}
    ids_[slot]=id;seenAt_[slot]=now;return true;
  }
  bool count(const NodeCache&cache,uint32_t self,uint64_t now,uint32_t&out){
    if(self<=1||self>=0xfffffffeu||!advance(now))return false;
    uint32_t count=1; // Known local node, even when there are no observations.
    for(unsigned i=0;i<64;i++){
      if(!ids_[i])continue;
      if(ids_[i]==self||now-seenAt_[i]>=Lifetime||cache.ignored(ids_[i])){ids_[i]=0;continue;}
      ++count;
    }
    out=count;return true;
  }
};
}
