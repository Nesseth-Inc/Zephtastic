// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TrafficManagementSettings.h"
#include "StockPerSourceRateWindow.h"
#include "StockPositionDedup.h"
#include "UnknownPacketWindow.h"
#include <pb_decode.h>
#include "meshtastic/mesh.pb.h"

namespace mt::modules {
// Bounded per-source ownership for the three pinned stock traffic gates.
// Full monotonic tick anchors expire compact counters before they can alias
// after long idle periods. No identity, forwarding, cache reply or RF authority.
class TrafficManagementWindow {
  struct Slot {
    uint32_t source=0;uint8_t cachedRole=0;
    uint64_t touched=0,rateStart=0,unknownStart=0,positionStart=0;
    traffic::StockPerSourceRateWindow rate;
    traffic::StockPositionDedup position;
    traffic::UnknownPacketWindow::State unknown;
    bool rateSeen=false,unknownSeen=false,positionSeen=false;
  };
  static constexpr unsigned Capacity=16;
  Slot slots_[Capacity]{};
  TrafficManagementSettings settings_;
  meshtastic_ModuleConfig_TrafficManagementConfig config_{};
  uint64_t last_=0;
  bool configured_=false;
  Slot &slot(uint32_t source,uint64_t now){
    unsigned oldest=0,empty=Capacity;
    for(unsigned i=0;i<Capacity;++i){
      if(slots_[i].source==source){slots_[i].touched=now;return slots_[i];}
      if(!slots_[i].source){if(empty==Capacity)empty=i;}
      else if(slots_[i].touched<slots_[oldest].touched)oldest=i;
    }
    // A removal can leave a hole before an existing source. Search the whole
    // array before allocating, or that source would acquire fresh duplicate state.
    const unsigned chosen=empty<Capacity?empty:oldest;
    slots_[chosen]=Slot{};slots_[chosen].source=source;slots_[chosen].touched=now;return slots_[chosen];
  }
  void advance(uint64_t now){
    if(now<last_)for(auto &s:slots_)s=Slot{};
    last_=now;
  }
  static uint64_t rateTicks(uint32_t seconds){const auto n=seconds/300;return n<1?1:n>15?15:n;}
  static uint64_t positionTicks(uint32_t seconds,uint32_t role){
    if(!seconds)return 0;
    if(role==uint32_t(meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND)&&seconds>900)seconds=900;
    if((role==uint32_t(meshtastic_Config_DeviceConfig_Role_TRACKER)||role==uint32_t(meshtastic_Config_DeviceConfig_Role_TAK_TRACKER))&&seconds>3600)seconds=3600;
    const uint64_t ticks=uint64_t(seconds)/360;return ticks<1?1:ticks>255?255:ticks;
  }
public:
  bool deduplicatesPosition() const { return configured_ && config_.position_min_interval_secs != 0; }
  bool configured() const { return configured_; }
  void configure(const TrafficManagementSettings &settings){
    if(!configured_||!settings.same(settings_)){
      for(auto &s:slots_)s=Slot{};
      settings_=settings;settings_.get(config_);configured_=true;
    }
  }
  void observeRole(uint32_t source,uint32_t role){
    if(!source)return;
    // Only an already tracked source can be updated by admitted NodeInfo.
    for(auto &s:slots_)if(s.source==source){s.cachedRole=uint8_t(role>15?15:role);return;}
  }
  bool forget(uint32_t source){
    if(!source)return false;
    for(auto &s:slots_)if(s.source==source){s=Slot{};return true;}
    return false;
  }
  bool unknown(uint32_t source,uint64_t now){
    if(!configured_||!source||!config_.unknown_packet_threshold)return false;
    advance(now);auto &s=slot(source,now);const uint64_t tick=now/60000;
    if(!s.unknownSeen||tick-s.unknownStart>=5){s.unknown={};s.unknownStart=tick;s.unknownSeen=true;}
    // The helper owns the low nibble; derive it from the full clock rather
    // than narrowing milliseconds (which would create a 49-day discontinuity).
    return traffic::UnknownPacketWindow::drop(s.unknown,uint32_t(tick&15)*60000,config_.unknown_packet_threshold);
  }
  bool decoded(uint32_t source,uint32_t destination,uint32_t self,uint32_t port,
               const uint8_t *payload,size_t length,bool wellKnownChannel,
               uint32_t precision,uint32_t role,uint64_t now,bool roleKnown=true,bool advisoryRole=false){
    if(!configured_||!source||source==self||destination==self)return false;
    advance(now);
    auto shouldDropPosition=[&](const meshtastic_Position &position) {
      auto &s=slot(source,now);const uint64_t tick=now/360000;
      // A known CLIENT resets a previous special-role cap. Eviction keeps the admitted cached role.
      if(roleKnown)s.cachedRole=uint8_t(role>15?15:role);
      // A warm hint expires with its own identity domain or withdrawal. Never
      // copy it into an independent, longer-lived traffic slot.
      const uint32_t effectiveRole=advisoryRole&&!roleKnown?role:s.cachedRole;
      if(!s.positionSeen||tick-s.positionStart>=positionTicks(config_.position_min_interval_secs,effectiveRole))
        s.position.reset();
      const bool drop=s.position.shouldDrop(true,true,position.latitude_i,position.longitude_i,precision,
        config_.position_min_interval_secs,effectiveRole,uint8_t(tick));
      if(!drop){s.positionStart=tick;s.positionSeen=true;}
      return drop;
    };
    auto isRateLimited=[&] {
      auto &s=slot(source,now);const uint64_t tick=now/300000;
      if(!s.rateSeen||tick-s.rateStart>=rateTicks(config_.rate_limit_window_secs)){
        s.rate.reset();s.rateStart=tick;s.rateSeen=true;
      }
      return !s.rate.allow(uint8_t(tick&15),config_.rate_limit_window_secs,config_.rate_limit_max_packets);
    };
    return stock::TrafficManagementModule::handleReceived(source,destination,self,port,payload,length,
      wellKnownChannel,config_,shouldDropPosition,isRateLimited);
  }
};
}
