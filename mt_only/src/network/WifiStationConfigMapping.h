// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "meshtastic/config.pb.h"
#include "WifiStationSettings.h"

// Bounded station-field admission; capability and runtime activation belong to the caller.
namespace mt { namespace network {
class WifiStationConfigMapping
{
public:
    static bool fromConfig(const meshtastic_Config_NetworkConfig &input,
                           bool boardWifiCapable,
                           const mt::network::WifiStationSettings &current,
                           mt::network::WifiStationSettings &out);

private:
    static bool boundedTerminated(const char *p, size_t size, size_t maxLen, size_t &len);
    static bool allZero(const void *p, size_t n);
};

inline bool WifiStationConfigMapping::boundedTerminated(const char *p, size_t size, size_t maxLen, size_t &len)
{
    size_t i = 0;
    while (i < size && p[i] != '\0')
        ++i;
    if (i == size)
        return false;
    len = i;
    if (len > maxLen)
        return false;
    for (size_t j = i + 1; j < size; ++j)
        if (p[j] != '\0')
            return false;
    return true;
}

inline bool WifiStationConfigMapping::allZero(const void *p, size_t n)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != 0)
            return false;
    return true;
}

inline bool WifiStationConfigMapping::fromConfig(const meshtastic_Config_NetworkConfig &input,
                                                 bool boardWifiCapable,
                                                 const mt::network::WifiStationSettings &current,
                                                 mt::network::WifiStationSettings &out)
{
    if (!current.valid())
        return false;
    size_t ntpLength=0;
    if (!boundedTerminated(input.ntp_server,sizeof(input.ntp_server),32,ntpLength)||(!boardWifiCapable&&ntpLength))
        return false;
    if (input.eth_enabled)
        return false;
    if (!allZero(&input.address_mode, sizeof(input.address_mode)))
        return false;
    if (input.has_ipv4_config)
        return false;
    if (input.ipv4_config.ip != 0 || input.ipv4_config.gateway != 0 ||
        input.ipv4_config.subnet != 0 || input.ipv4_config.dns != 0)
        return false;
    if (!allZero(input.rsyslog_server, sizeof(input.rsyslog_server)))
        return false;
    if (input.enabled_protocols != 0)
        return false;
    if (input.ipv6_enabled)
        return false;
    size_t ssidLen = 0;
    size_t pskLen = 0;
    if (!boundedTerminated(input.wifi_ssid, sizeof(input.wifi_ssid), 32, ssidLen))
        return false;
    if (!boundedTerminated(input.wifi_psk, sizeof(input.wifi_psk), 64, pskLen))
        return false;
    if (input.wifi_enabled && !boardWifiCapable)
        return false;
    // Pinned stock AdminModule reserves this public placeholder to retain
    // the existing PSK. A real empty string clears it, even when disabled.
    const char *psk = input.wifi_psk;
    if (pskLen == 6 && memcmp(psk, "sekrit", 6) == 0) {
        psk = reinterpret_cast<const char *>(current.bytes + 41);
        pskLen = current.pskLength();
    }
    mt::network::WifiStationSettings staged = current;
    if (!staged.set(input.wifi_enabled, input.wifi_ssid, ssidLen, psk, pskLen))
        return false;
    if(!staged.setNtp(input.ntp_server,ntpLength))return false;
    out = staged;
    return true;
}
} } // namespace mt::network
