// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <string.h>

#include "meshtastic/config.pb.h"
#include "meshtastic/module_config.pb.h"

namespace mt::admin::remote_redaction {

constexpr char ReservedSecret[] = "sekrit";

// Stock remote getters replace a secret string with "sekrit" and zero-fill
// the fixed-capacity nanopb field. Reject unterminated source fields before
// changing them so malformed configuration never yields a partial reply.
template <size_t N>
inline bool redactReservedString(char (&value)[N]) noexcept {
    if (N < sizeof(ReservedSecret) ||
        memchr(value, '\0', N) == nullptr) {
        return false;
    }
    memset(value, 0, N);
    memcpy(value, ReservedSecret, sizeof(ReservedSecret));
    return true;
}

inline bool validOptionalKeyLength(pb_size_t size, size_t capacity) noexcept {
    return size == 0 || size == capacity;
}

inline bool validSecurityForRemoteReply(
    const meshtastic_Config_SecurityConfig &security) noexcept {
    if (!validOptionalKeyLength(security.public_key.size,
                                sizeof(security.public_key.bytes)) ||
        !validOptionalKeyLength(security.private_key.size,
                                sizeof(security.private_key.bytes)) ||
        security.admin_key_count >
            sizeof(security.admin_key) / sizeof(security.admin_key[0])) {
        return false;
    }
    for (pb_size_t i = 0; i < security.admin_key_count; ++i) {
        if (security.admin_key[i].size != sizeof(security.admin_key[i].bytes)) {
            return false;
        }
    }
    return true;
}

inline bool redactRemoteWifiPsk(meshtastic_Config &config) noexcept {
    if (config.which_payload_variant != meshtastic_Config_network_tag) {
        return false;
    }
    return redactReservedString(config.payload_variant.network.wifi_psk);
}

inline bool redactRemoteSecurityPrivateKey(meshtastic_Config &config) noexcept {
    if (config.which_payload_variant != meshtastic_Config_security_tag) {
        return false;
    }
    auto &security = config.payload_variant.security;
    if (!validSecurityForRemoteReply(security)) {
        return false;
    }

    // Wipe the full generated capacity, not just the declared length, before
    // clearing the length so no stale private-key bytes remain in the reply.
    volatile pb_byte_t *bytes = security.private_key.bytes;
    for (size_t i = 0; i < sizeof(security.private_key.bytes); ++i) {
        bytes[i] = 0;
    }
    security.private_key.size = 0;
    return true;
}

inline bool redactRemoteMqttPassword(meshtastic_ModuleConfig &config) noexcept {
    if (config.which_payload_variant != meshtastic_ModuleConfig_mqtt_tag) {
        return false;
    }
    return redactReservedString(config.payload_variant.mqtt.password);
}

} // namespace mt::admin::remote_redaction
