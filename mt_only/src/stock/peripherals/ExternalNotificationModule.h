// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/ExternalNotificationModule.cpp
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "meshtastic/portnums.pb.h"

#include "../message/TextMessageModule.h"
namespace mt::stock::notification {

// ExternalNotificationModule::wantPacket delegates to MeshService::isTextPayload.
inline bool wantPacket(uint32_t port, bool rangeTestEnabled)
{
    return text::wantPacket(port, rangeTestEnabled);
}

struct Flags {
    bool alertBell, alertMessage;
    bool alertBellVibra, alertMessageVibra;
    bool alertBellBuzzer, alertMessageBuzzer;
};
struct Outputs {
    bool generic, vibra, buzzer;
};

// handleReceived, after enablement, local-sender and admission checks.
// DeviceConfig on ZT accepts the stock default buzzer mode. The physical
// output owners provide their capabilities and latch independently.
inline Outputs handleReceived(const Flags &flags, const uint8_t *payload, size_t size,
                              bool muted, bool canBuzz, bool buzzerShouldAlert)
{
    bool containsBell = false;
    for (size_t i = 0; i < size; ++i) {
        if (payload[i] == 7) {
            containsBell = true;
            break;
        }
    }
    const bool genericShouldAlert = (flags.alertBell && containsBell) || (flags.alertMessage && !muted);
    const bool vibraShouldAlert = (flags.alertBellVibra && containsBell) || (flags.alertMessageVibra && !muted);
    buzzerShouldAlert = buzzerShouldAlert || (canBuzz && ((flags.alertBellBuzzer && containsBell) ||
                                                          (flags.alertMessageBuzzer && !muted)));
    return {genericShouldAlert, vibraShouldAlert, buzzerShouldAlert};
}

inline uint32_t armNagCycle(uint32_t now, uint32_t nagTimeout, uint32_t outputMs)
{
    const uint32_t durationMs = nagTimeout ? nagTimeout * 1000u : outputMs;
    return now + durationMs;
}

// ExternalNotificationModule::handleInputEvent consumes the first local input while armed.
template <class Stop> inline bool handleInputEvent(bool isNagging, Stop stopNow)
{
    if (!isNagging)
        return false;
    stopNow();
    return true;
}

inline bool deadlinePassed(uint32_t now, uint32_t cutoff)
{
    return uint32_t(now - cutoff) < 0x80000000u;
}

// GPIO toggling and ringtone scheduling use different clocks in runOnce.
inline bool outputDue(uint32_t now, uint32_t last, uint32_t interval)
{
    return uint32_t(now - last) >= interval;
}
} // namespace mt::stock::notification
