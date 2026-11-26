// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../Config.h"
#include "../stock/modules/PositionModule.h"
#include "meshtastic/module_config.pb.h"

namespace mt::modules {
// Portable payload for the new, separate module-extension journal. The old
// 112-byte module record and every identity/key/settings record stay intact.
// Store raw uint32 values: pinned stock saturates seconds at runtime, so the
// codec must preserve explicit zero and UINT32_MAX rather than normalize them.
struct TrafficManagementSettings {
  static constexpr size_t Size=20;
  static constexpr uint32_t InitialPositionIntervalSeconds=stock::position::DefaultDedupSeconds;
  uint8_t bytes[Size]{};
  TrafficManagementSettings(){Config::put32(bytes,InitialPositionIntervalSeconds);}
  void get(meshtastic_ModuleConfig_TrafficManagementConfig &out)const {
    out={};
    out.position_min_interval_secs=Config::get32(bytes);
    out.nodeinfo_direct_response_max_hops=Config::get32(bytes+4);
    out.rate_limit_window_secs=Config::get32(bytes+8);
    out.rate_limit_max_packets=Config::get32(bytes+12);
    out.unknown_packet_threshold=Config::get32(bytes+16);
  }
  void set(const meshtastic_ModuleConfig_TrafficManagementConfig &in){
    Config::put32(bytes,in.position_min_interval_secs);
    Config::put32(bytes+4,in.nodeinfo_direct_response_max_hops);
    Config::put32(bytes+8,in.rate_limit_window_secs);
    Config::put32(bytes+12,in.rate_limit_max_packets);
    Config::put32(bytes+16,in.unknown_packet_threshold);
  }
  bool same(const TrafficManagementSettings &other)const{return !memcmp(bytes,other.bytes,Size);}
};
static_assert(sizeof(TrafficManagementSettings)==TrafficManagementSettings::Size);
}
