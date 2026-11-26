// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/esp32/PaxcounterModule.cpp
#pragma once
#include <stdint.h>
#include "meshtastic/paxcount.pb.h"

namespace mt::stock::paxcounter {
// The ZT submission callback replaces stock allocation and sendToMesh. Both
// execute in the module owner, after a passive observer supplies fresh counts.
inline meshtastic_Paxcount allocReply(uint32_t wifi, uint32_t ble, uint32_t uptimeSeconds) {
  meshtastic_Paxcount count = meshtastic_Paxcount_init_default;
  count.wifi = wifi;
  count.ble = ble;
  count.uptime = uptimeSeconds;
  return count;
}

template<typename Submit>
bool sendInfo(bool &reportedDataSent, uint32_t wifi, uint32_t ble, uint32_t uptimeSeconds, Submit &submit) {
  if (reportedDataSent) return false;

  const auto count = allocReply(wifi, ble, uptimeSeconds);
  if (!submit(count)) return false;

  reportedDataSent = true;
  return true;
}
}
