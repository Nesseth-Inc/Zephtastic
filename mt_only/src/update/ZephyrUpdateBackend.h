// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include "meshtastic/config.pb.h"

namespace mt::update {
enum class Mode:uint8_t { None,NordicUf2,NordicBleDfu,EspBleLoader,EspWifiLoader };

// Capability query has no writes. prepare() is called exactly once by the
// owner only after storage/radio/client quiescence; it stages boot handoff but
// never resets. rebootPrepared() is called only after prepare() succeeds.
bool available(Mode mode);
int prepare(Mode mode,const uint8_t hash[32],const meshtastic_Config_NetworkConfig &network);
void rebootPrepared(Mode mode);
}
