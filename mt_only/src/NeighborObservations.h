// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "NodeCache.h"
#include "ModuleSettings.h"
#include "stock/modules/NeighborTable.h"
namespace mt {
// Volatile NeighborInfo observations. Wire timestamps never drive freshness;
// the caller supplies local monotonic time and direct RF status separately.
class NeighborObservations {
  stock::NeighborTable neighbors_;
  static bool validId(uint32_t id,uint32_t self){return id>1&&id<0xfffffffeu&&id!=self;}
  static bool validSample(bool known,float snr){return !known||(snr>=-128&&snr<=127);}
public:
  void clear(){neighbors_.clear();}
  bool observe(uint32_t id,uint32_t self,unsigned hops,uint64_t now,bool snrKnown,float snr,uint32_t localIntervalSeconds=0){
    if(!validId(id,self)||hops||now==UINT64_MAX||!validSample(snrKnown,snr)||
       !ModuleSettings::stockInterval(localIntervalSeconds))return false;
    neighbors_.getOrCreateNeighbor(id,id,0,localIntervalSeconds,snrKnown?snr:0,now,true);
    return true;
  }
  // The caller supplies the last transmitter resolved from the radio header.
  // A peer-listed edge alone is never a direct RF observation.
  bool observeAnnouncement(uint32_t source,uint32_t self,uint32_t lastSentBy,
                          uint32_t advertisedIntervalSeconds,uint32_t localIntervalSeconds,unsigned hops,
                          uint64_t now,bool snrKnown,float snr){
    if(!validId(source,self)||now==UINT64_MAX||!validSample(snrKnown,snr)||
       !ModuleSettings::stockInterval(advertisedIntervalSeconds)||
       !ModuleSettings::stockInterval(localIntervalSeconds))return false;
    const uint32_t id=lastSentBy?lastSentBy:self;
    if(id==self)return true; // Stock maps zero to self; outbound reports omit self.
    if(!validId(id,self))return false;
    neighbors_.getOrCreateNeighbor(source,id,advertisedIntervalSeconds,localIntervalSeconds,
                                   snrKnown?snr:0,now,source==id&&hops==0);
    return true;
  }
  bool snapshot(const NodeCache&cache,uint32_t self,uint32_t interval,uint64_t now,meshtastic_NeighborInfo&out){
    out={};if(self<=1||self>=0xfffffffeu||!interval||!ModuleSettings::stockInterval(interval)||now==UINT64_MAX)return false;
    neighbors_.collectNeighborInfo(out,self,interval,now,[&](uint32_t node){return cache.ignored(node);});
    return out.neighbors_count!=0;
  }
  bool direct(uint32_t id)const{return neighbors_.direct(id);}
};
}
