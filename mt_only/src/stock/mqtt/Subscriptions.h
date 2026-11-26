// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, MQTT::sendSubscriptions in src/mqtt/MQTT.cpp.
#pragma once
#include <stddef.h>

namespace mt::stock::MQTT {
// The caller resolves the effective global channel ID and owns the topic
// buffer. The broker adapter installs these encrypted filters at QoS 1.
template<class DownlinkEnabled, class GlobalId, class Subscribe>
bool sendSubscriptions(size_t numChannels, DownlinkEnabled downlinkEnabled,
                       GlobalId getGlobalId, Subscribe subscribe) {
    bool hasDownlink = false;
    for (size_t i = 0; i < numChannels; ++i) {
        if (downlinkEnabled(i)) {
            hasDownlink = true;
            if (!subscribe(getGlobalId(i))) return false;
        }
    }
    return !hasDownlink || subscribe("PKI");
}
}
