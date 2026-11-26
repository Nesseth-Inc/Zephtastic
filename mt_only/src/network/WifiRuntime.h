// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "WifiStationSettings.h"
namespace mt::network {
struct WifiStatus {bool connected=false;bool mqttReady=false;uint32_t ip=0;char ssid[33]{};};
// Main thread only, outside serviceLock. Driver callbacks only publish atomic
// observations. The owned credential snapshot never changes during this boot.
bool wifiAvailable();
bool wifiBootActive();
bool wifiStart(const WifiStationSettings &settings,bool settingsReady);
void wifiPoll();
WifiStatus wifiStatus();
// Main owner consumes a successful SNTP result under serviceLock. DHCP ready
// alone is never a time source, and no callback changes Service/WallClock.
bool wifiTakeTime(uint32_t &seconds);
}
