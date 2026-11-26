// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Stateless wire-shape classification for a deliberately narrow remote Admin
// mutation slice. This is not authentication, authorization, or mutation
// policy. The caller must prove the current peer/key, validate the
// session-passkey value, and for favorite operations prove local ownership
// policy before applying a typed setter. SecurityConfig shape checks here do
// not establish key ownership or unchanged allowlist/public-key state; the
// owner must compare those fields with its active persisted configuration.
#include <stddef.h>
#include <stdint.h>

#include "admin/RemoteAdminProtocol.h"

namespace mt::admin::mutation_protocol {

constexpr uint8_t MaxChannelIndex = 7;  // Pinned stock MAX_NUM_CHANNELS is 8.

// Metadata safe to retain: no passkey, peer/key, auth state, or setter body.
enum class Operation : uint8_t {
    Invalid = 0, Owner, Config, ModuleConfig, Channel, Favorite, Unfavorite,
    Ignore, Unignore, ToggleMute, SetTimeOnly, StoreUiConfig, SetCannedMessages, SetRingtone,
    FixedPosition, RemoveFixedPosition, Reboot, Shutdown, Ota, Dfu
};
struct Mutation {
    Operation operation = Operation::Invalid;
    uint8_t selector = 0;  // Config/ModuleConfig oneof tag or zero-based channel index.
    uint32_t node = 0;     // Full node number for node policy; zero for other operations.
};

inline bool validFavoriteNode(uint32_t node) noexcept {
    return node != 0u && node != 1u && node != 0xfffffffeu && node != 0xffffffffu;
}

// These are wire shapes with a concrete MainHost save path. This list does
// not imply that an optional backend is compiled or available; the owner must
// still perform capability and typed-settings validation before saving.
inline bool supportedModuleConfigTag(uint8_t tag) noexcept {
    switch (tag) {
    case meshtastic_ModuleConfig_audio_tag:
    case meshtastic_ModuleConfig_ambient_lighting_tag:
    case meshtastic_ModuleConfig_paxcounter_tag:
    case meshtastic_ModuleConfig_telemetry_tag:
    case meshtastic_ModuleConfig_external_notification_tag:
    case meshtastic_ModuleConfig_serial_tag:
    case meshtastic_ModuleConfig_canned_message_tag:
    case meshtastic_ModuleConfig_range_test_tag:
    case meshtastic_ModuleConfig_mqtt_tag:
    case meshtastic_ModuleConfig_neighbor_info_tag:
    case meshtastic_ModuleConfig_statusmessage_tag:
    case meshtastic_ModuleConfig_traffic_management_tag:
    case meshtastic_ModuleConfig_mesh_beacon_tag:
    case meshtastic_ModuleConfig_store_forward_tag:
    case meshtastic_ModuleConfig_remote_hardware_tag:
    case meshtastic_ModuleConfig_detection_sensor_tag:
        return true;
    default:
        return false;
    }
}

inline bool supportedConfigSetTag(uint8_t tag) noexcept {
    return tag == meshtastic_Config_device_tag ||
           tag == meshtastic_Config_position_tag ||
           tag == meshtastic_Config_power_tag ||
           tag == meshtastic_Config_network_tag ||
           tag == meshtastic_Config_display_tag ||
           tag == meshtastic_Config_lora_tag ||
           tag == meshtastic_Config_bluetooth_tag ||
           tag == meshtastic_Config_security_tag;
}

inline bool validMutation(const Mutation &mutation) noexcept {
    switch (mutation.operation) {
    case Operation::Owner:
        return mutation.selector == 0 && mutation.node == 0;
    case Operation::Config:
        return mutation.node == 0 && supportedConfigSetTag(mutation.selector);
    case Operation::ModuleConfig:
        return mutation.node == 0 && supportedModuleConfigTag(mutation.selector);
    case Operation::Channel:
        return mutation.node == 0 && mutation.selector <= MaxChannelIndex;
    case Operation::Favorite:
    case Operation::Unfavorite:
    case Operation::Ignore:
    case Operation::Unignore:
    case Operation::ToggleMute:
        return mutation.selector == 0 && validFavoriteNode(mutation.node);
    case Operation::FixedPosition:
    case Operation::RemoveFixedPosition:
    case Operation::SetTimeOnly:
    case Operation::StoreUiConfig:
    case Operation::SetCannedMessages:
    case Operation::SetRingtone:
    case Operation::Reboot:
    case Operation::Shutdown:
    case Operation::Ota:
    case Operation::Dfu:
        return mutation.selector == 0 && mutation.node == 0;
    default:
        return false;
    }
}

// Static SET-shape constraints for the narrow, future managed/serial/signature
// policy operation. Current key material is read-only and must be compared by
// the owner against its active persisted SecurityConfig before applying.
inline bool validSecuritySetShape(const meshtastic_Config_SecurityConfig &security) noexcept {
    if (security.private_key.size != 0 ||
        (security.public_key.size != 0 && security.public_key.size != 32) ||
        security.admin_key_count > 3)
        return false;
    for (pb_size_t i = 0; i < security.admin_key_count; ++i)
        if (security.admin_key[i].size != 32) return false;
    uint32_t policy = 0;
    return remote_protocol::enumNumber(security.packet_signature_policy, policy) && policy <= 2;
}

// Accepts only canonical Admin SET plaintext carried in canonical ADMIN_APP
// Data. The shared request-envelope validator is reused for stock
// request framing: wantsResponse must be true either through the legacy bool
// or bitfield bit 1, Data.request_id is zero, and only defined bitfield bits
// are allowed. It does not require GET-specific operation fields. The
// eight-byte passkey is checked for shape only, and all decoded body/passkey
// scratch is wiped before return. Authentication and passkey comparison are
// exclusively the caller's responsibility.
inline bool canonicalSetRequest(const uint8_t *wire, size_t length,
                                Mutation &result) noexcept {
    uint8_t payload[remote_protocol::MaxPayloadBytes]{};
    remote_protocol::Wipe wipePayload{payload, sizeof(payload)};
    size_t payloadSize = 0;
    if (!remote_protocol::extractCanonicalPayload(wire, length, true, 0,
                                                   payload, payloadSize))
        return false;

    meshtastic_AdminMessage admin{};
    remote_protocol::Wipe wipeAdmin{&admin, sizeof(admin)};
    uint8_t canonicalAdmin[remote_protocol::MaxPayloadBytes]{};
    remote_protocol::Wipe wipeCanonical{canonicalAdmin, sizeof(canonicalAdmin)};
    if (!remote_protocol::decodeCanonicalAdmin(payload, payloadSize, admin, canonicalAdmin) ||
        admin.session_passkey.size != remote_protocol::SessionPasskeyBytes)
        return false;

    Mutation candidate{};
    switch (admin.which_payload_variant) {
    case meshtastic_AdminMessage_set_owner_tag:
        candidate.operation = Operation::Owner;
        break;
    case meshtastic_AdminMessage_set_config_tag:
        if (admin.set_config.which_payload_variant > 0xffu ||
            !supportedConfigSetTag(static_cast<uint8_t>(admin.set_config.which_payload_variant)))
            return false;
        if (admin.set_config.which_payload_variant == meshtastic_Config_security_tag &&
            !validSecuritySetShape(admin.set_config.payload_variant.security)) return false;
        candidate.operation = Operation::Config;
        candidate.selector = static_cast<uint8_t>(admin.set_config.which_payload_variant);
        break;
    case meshtastic_AdminMessage_set_module_config_tag:
        if (admin.set_module_config.which_payload_variant > 0xffu ||
            !supportedModuleConfigTag(static_cast<uint8_t>(admin.set_module_config.which_payload_variant)))
            return false;
        candidate.operation = Operation::ModuleConfig;
        candidate.selector = static_cast<uint8_t>(admin.set_module_config.which_payload_variant);
        break;
    case meshtastic_AdminMessage_set_channel_tag:
        if (admin.set_channel.index < 0 || admin.set_channel.index > MaxChannelIndex)
            return false;
        candidate.operation = Operation::Channel;
        candidate.selector = static_cast<uint8_t>(admin.set_channel.index);
        break;
    case meshtastic_AdminMessage_set_favorite_node_tag:
        candidate.operation = Operation::Favorite;
        candidate.node = admin.set_favorite_node;
        break;
    case meshtastic_AdminMessage_remove_favorite_node_tag:
        candidate.operation = Operation::Unfavorite;
        candidate.node = admin.remove_favorite_node;
        break;
    case meshtastic_AdminMessage_set_ignored_node_tag:
        candidate.operation=Operation::Ignore;candidate.node=admin.set_ignored_node;break;
    case meshtastic_AdminMessage_remove_ignored_node_tag:
        candidate.operation=Operation::Unignore;candidate.node=admin.remove_ignored_node;break;
    case meshtastic_AdminMessage_toggle_muted_node_tag:
        candidate.operation=Operation::ToggleMute;candidate.node=admin.toggle_muted_node;break;
    case meshtastic_AdminMessage_set_time_only_tag:
        candidate.operation=Operation::SetTimeOnly;break;
    case meshtastic_AdminMessage_store_ui_config_tag:
        candidate.operation=Operation::StoreUiConfig;break;
    case meshtastic_AdminMessage_set_canned_message_module_messages_tag:
        candidate.operation=Operation::SetCannedMessages;break;
    case meshtastic_AdminMessage_set_ringtone_message_tag:
        candidate.operation=Operation::SetRingtone;break;
    case meshtastic_AdminMessage_set_fixed_position_tag:
        candidate.operation=Operation::FixedPosition;break;
    case meshtastic_AdminMessage_remove_fixed_position_tag:
        candidate.operation=Operation::RemoveFixedPosition;break;
    case meshtastic_AdminMessage_reboot_seconds_tag:
        candidate.operation=Operation::Reboot;break;
    case meshtastic_AdminMessage_shutdown_seconds_tag:
        candidate.operation=Operation::Shutdown;break;
    case meshtastic_AdminMessage_ota_request_tag:
        candidate.operation=Operation::Ota;break;
    case meshtastic_AdminMessage_enter_dfu_mode_request_tag:
        candidate.operation=Operation::Dfu;break;
    default:
        return false;
    }
    if (!validMutation(candidate)) return false;
    result = candidate;
    return true;
}

}  // namespace mt::admin::mutation_protocol
