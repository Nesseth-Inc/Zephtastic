// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic d5f16191, MeshModule::setStartDelay and PositionModule constructor.
#pragma once
#include <stdint.h>
#include "meshtastic/config.pb.h"

namespace mt::stock {
constexpr uint32_t periodicStartMs(unsigned priorOwners) {return 30000u+15000u*priorOwners;}
constexpr bool trackerSkipsPositionStagger(unsigned role) {
  return role==meshtastic_Config_DeviceConfig_Role_TRACKER||role==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER;
}
}
