// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/RangeTestModule.cpp
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace mt::stock {

struct RangeTestModuleRadio {
    template <class Send> static bool sendPayload(uint32_t packetSequence, Send sendToMesh)
    {
        if (!packetSequence)
            return false;
        char heartbeatString[16];
        const int length = snprintf(heartbeatString, sizeof(heartbeatString), "seq %u",
                                    static_cast<unsigned>(packetSequence));
        if (length <= 0 || size_t(length) >= sizeof(heartbeatString))
            return false;
        return sendToMesh(reinterpret_cast<const uint8_t *>(heartbeatString), size_t(length));
    }

    template <class Append>
    static void handleReceived(bool enabled, bool save, bool isFromUs, Append appendFile)
    {
        if (enabled && !isFromUs && save)
            appendFile();
    }
};

class RangeTestModule {
public:
    static constexpr uint32_t FirstDelayMs = 5000;

    void start(uint64_t now) { started = now; packetSequence = 0; }

    // ZT's monotonic scheduler rejects an expired heartbeat before transmitting and
    // preserves the local phone copy even when the RF queue refuses the broadcast.
    template <class Send> bool runOnce(uint64_t now, bool channelAllowed, Send sendToMesh)
    {
        if (now < started || now - started >= 28800000 || packetSequence == UINT32_MAX)
            return false;
        if (channelAllowed)
            RangeTestModuleRadio::sendPayload(++packetSequence, sendToMesh);
        return true;
    }

private:
    uint64_t started = 0;
    uint32_t packetSequence = 0;
};
}
