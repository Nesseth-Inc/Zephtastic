// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "GnssSettings.h"
#include "meshtastic/mesh.pb.h"
namespace mt::gnss {
// Fitted capability is stable across startup, suspend and device faults.
// Runtime diagnostics are a separate, mutex-published scalar snapshot.
struct Diagnostics {
  bool initialized=false,ready=false,fault=false,acquiring=false;
  uint32_t sentences=0,callbacks=0,promotions=0;
  uint16_t satellites=0;
};
// Main-thread nonblocking request/snapshot API. Dedicated worker owns power
// transitions; native callback publishes immutable fixes under its own mutex.
#if defined(CONFIG_MT_GNSS_T096) || defined(CONFIG_MT_GNSS_XIAO_L76K)
inline constexpr bool present(){return true;}
Diagnostics diagnostics();
void poll(const GnssSettings &settings,bool fixed,uint64_t now);
meshtastic_Position snapshot(uint64_t now);
uint32_t utcSnapshot(uint64_t now);
#else
inline bool present(){return false;}
inline Diagnostics diagnostics(){return {};}
inline void poll(const GnssSettings &,bool,uint64_t){}
inline meshtastic_Position snapshot(uint64_t){return {};}
inline uint32_t utcSnapshot(uint64_t){return 0;}
#endif
}
