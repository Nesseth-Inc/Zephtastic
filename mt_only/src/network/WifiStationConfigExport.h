// SPDX-License-Identifier: GPL-3.0-only
// Local corrected candidate based on the exact WifiStationSettings API.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "meshtastic/config.pb.h"
#include "network/WifiStationSettings.h"

namespace mt {
namespace network {

class WifiStationConfigExport {
public:
    static bool toConfig(const WifiStationSettings &input,
                         meshtastic_Config_NetworkConfig &out) {
        if (!input.valid()) return false;
        const uint8_t ssidLen = input.ssidLength();
        const uint8_t pskLen = input.pskLength();
        if (ssidLen > 32u || pskLen > 64u) return false;

        meshtastic_Config_NetworkConfig candidate =
            meshtastic_Config_NetworkConfig_init_zero;
        candidate.wifi_enabled = input.enabled();
        memcpy(candidate.wifi_ssid, input.bytes + 8, ssidLen);
        candidate.wifi_ssid[ssidLen] = '\0';
        memcpy(candidate.wifi_psk, input.bytes + 41, pskLen);
        candidate.wifi_psk[pskLen] = '\0';
        if(input.NtpSize)memcpy(candidate.ntp_server,input.ntpServer(),input.NtpSize);
        out = candidate;
        return true;
    }
};

}  // namespace network
}  // namespace mt
