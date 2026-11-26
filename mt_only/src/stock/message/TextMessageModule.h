// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d TextMessageModule, MeshService and Screen wake policy.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "meshtastic/portnums.pb.h"
#include "meshtastic/config.pb.h"

namespace mt::stock::text {
struct Settings {
    bool rangeEnabled = false;
    bool externalNotifications = false;
    bool bellOutput = false;
    uint32_t role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    int batteryPercent = -1;
};

inline bool wantPacket(uint32_t port, bool rangeEnabled)
{
    return port == meshtastic_PortNum_TEXT_MESSAGE_APP || port == meshtastic_PortNum_DETECTION_SENSOR_APP ||
           port == meshtastic_PortNum_ALERT_APP || (rangeEnabled && port == meshtastic_PortNum_RANGE_TEST_APP);
}

inline bool shouldWake(const Settings &settings)
{
    const auto role = settings.role;
    return !settings.externalNotifications &&
           (role == meshtastic_Config_DeviceConfig_Role_CLIENT || role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE ||
            role == meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN || role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) &&
           (settings.batteryPercent < 0 || settings.batteryPercent >= 10);
}

// ZT stores sanitized previews and de-duplicates by sender and packet ID across
// companion fanout. An already stored message cannot extend the screen timer.
template<class Store, class Wake>
void handleReceived(unsigned port, const uint8_t *payload, size_t size, bool muted, bool local,
                    const Settings &settings, Store store, Wake wake)
{
    if (!wantPacket(port, settings.rangeEnabled) || !store()) return;
    const bool alert = settings.bellOutput && payload && memchr(payload, 7, size);
    if (local || (shouldWake(settings) && (!muted || alert))) wake();
}
} // namespace mt::stock::text
