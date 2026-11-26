// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "../stock/modules/PositionModule.h"
#include <math.h>

namespace mt {
// Pure cadence state. The caller owns consent, valid current position, role/
// network interval resolution, precision-cell distance and send admission.
class PositionSchedule {
public:
  enum class Kind:uint8_t {None,Periodic,Smart};
  static constexpr uint32_t MaxIntervalMs=0x7fffffffu;
  static constexpr uint32_t CompanionDefaultMs=3600000,RouterDefaultMs=43200000;
  static constexpr uint32_t StationaryFloorMs=stock::position::StationaryBroadcastSeconds*1000,SmartDefaultMs=300000,SmartDefaultDistanceM=100;
  struct Parameters {
    uint32_t regularIntervalMs=CompanionDefaultMs;
    uint32_t smartMinimumIntervalMs=SmartDefaultMs;
    uint32_t smartMinimumDistanceM=SmartDefaultDistanceM;
    bool smartEnabled=false,stationary=false;
  };
private:
  uint32_t lastSendMs_=0;
  int32_t latitude_=0,longitude_=0;
  static bool interval(uint32_t ms){return ms&&ms<=MaxIntervalMs;}
public:
  void restoreLastSend(uint32_t stamp){lastSendMs_=stamp;}
  uint32_t lastSendMs()const{return lastSendMs_;}
  int32_t lastLatitude()const{return latitude_;}
  int32_t lastLongitude()const{return longitude_;}
  static uint32_t secondsToMs(uint32_t configured,uint32_t fallbackSeconds){
    const uint32_t seconds=configured?configured:fallbackSeconds;
    return seconds>MaxIntervalMs/1000?MaxIntervalMs:seconds*1000;
  }
  static uint32_t distanceThreshold(uint32_t configured){return configured?configured:SmartDefaultDistanceM;}
  bool periodicDue(uint32_t now,const Parameters &p)const{
    if(!interval(p.regularIntervalMs))return false;
    const uint32_t wait=stock::position::effectiveBroadcastIntervalMs(p.regularIntervalMs,p.stationary,StationaryFloorMs);
    return lastSendMs_==0||uint32_t(now-lastSendMs_)>=wait;
  }
  bool smartDue(uint32_t now,const Parameters &p,double movedMeters)const{
    return p.smartEnabled&&interval(p.smartMinimumIntervalMs)&&isfinite(movedMeters)&&movedMeters>=0&&
      movedMeters>=distanceThreshold(p.smartMinimumDistanceM)&&uint32_t(now-lastSendMs_)>=p.smartMinimumIntervalMs;
  }
  // periodicOpportunity=false represents stock handleNewPosition's independent
  // smart-only entry. Smart does not require a previous valid broadcast fix.
  Kind decide(uint32_t now,const Parameters &p,double movedMeters,bool periodicOpportunity=true)const{
    if(periodicOpportunity&&periodicDue(now,p))return Kind::Periodic;
    return smartDue(now,p,movedMeters)?Kind::Smart:Kind::None;
  }
  // Call only after a send succeeds. Preserve stock's asymmetric sentinel:
  // periodic stores literal0; smart skipZero(0) stores1. A periodic success at
  // time0 is still "first" on the next tick; no silent sentinel repair here.
  bool stampSuccess(Kind kind,uint32_t now,int32_t latitude,int32_t longitude){
    if(kind!=Kind::Periodic&&kind!=Kind::Smart)return false;
    lastSendMs_=kind==Kind::Smart&&!now?1:now;latitude_=latitude;longitude_=longitude;return true;
  }
};
}
