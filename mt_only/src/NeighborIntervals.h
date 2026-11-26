// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ModuleIntervals.h"
namespace mt { namespace NeighborIntervals {
// Stock has three intentionally different NeighborInfo interval call sites.
inline uint32_t initialMs(uint32_t configuredSeconds,bool router) {
  return ModuleIntervals::base(configuredSeconds,router);
}
inline uint32_t advertisedSeconds(uint32_t configuredSeconds,bool router) {
  return configuredSeconds?configuredSeconds:(router?43200u:3600u);
}
inline uint32_t steadyMs(uint32_t configuredSeconds) {
  return ModuleIntervals::base(configuredSeconds?configuredSeconds:21600u,false);
}
} }
