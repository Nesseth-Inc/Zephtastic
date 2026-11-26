// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "WifiStationSettings.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt { namespace network {
class WifiBootSnapshot final {
    char ssid_[33]{};
    char psk_[65]{};
#if defined(CONFIG_MT_WIFI_STATION)
    char ntp_[WifiStationSettings::NtpSize]{};
#endif
    bool prepared_=false;
    bool active_=false;
public:
    WifiBootSnapshot()=default;
    WifiBootSnapshot(const WifiBootSnapshot&)=delete;
    WifiBootSnapshot& operator=(const WifiBootSnapshot&)=delete;
    WifiBootSnapshot(WifiBootSnapshot&&)=delete;
    WifiBootSnapshot& operator=(WifiBootSnapshot&&)=delete;
    bool prepare(const WifiStationSettings& record,bool boardCapable) {
        // One-shot: once prepared, any further call fails with all bytes unchanged.
        if (prepared_) {
            return false;
        }
        // Invalid record fails unchanged (enabled() is guarded by valid()).
        if (!record.valid()) {
            return false;
        }
        const bool enabled = record.enabled();
        // Enabled record on an incapable board fails unchanged.
        if (enabled && !boardCapable) {
            return false;
        }

        // All validation passed; commit copy-on-success into object-owned buffers.
        if (enabled) {
            // Layout: SSID at bytes 8..40 (<=32 + NUL), PSK at bytes 41..105 (<=64 + NUL).
            constexpr size_t kSsidOffset = 8;
            constexpr size_t kPskOffset = 41;
            const size_t s = record.ssidLength();
            const size_t p = record.pskLength();
            // Copy exactly the validated bytes; lengths are already validated (<=32/<=64).
            memcpy(ssid_, record.bytes + kSsidOffset, s);
            ssid_[s] = '\0';
            // Empty PSK (p == 0) remains empty for open networks.
            memcpy(psk_, record.bytes + kPskOffset, p);
            psk_[p] = '\0';
#if defined(CONFIG_MT_WIFI_STATION)
            memcpy(ntp_, record.ntpServer(), sizeof(ntp_));
#endif
            active_ = true;
        } else {
            // Valid disabled record: prepared, inactive, empty NUL-terminated buffers.
            ssid_[0] = '\0';
            psk_[0] = '\0';
            active_ = false;
        }
        prepared_ = true;
        return true;
    }
    bool prepared()const{return prepared_;}
    bool active()const{return active_;}
    const char *ssid()const{return ssid_;}
    const char *psk()const{return psk_;}
    const char *ntpServer()const{
#if defined(CONFIG_MT_WIFI_STATION)
        if (*ntp_) return ntp_;
#endif
        return "meshtastic.pool.ntp.org";
    }
};
} }
