// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Stateless, bounded wire-shape validation for a narrow RF Admin GET slice.
// This does not authenticate or authorize a peer and must only be called by a
// dispatcher that has already verified the PrivateEvent identity and PKI path.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include "../stock/admin/AdminModule.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"

namespace mt::admin::remote_protocol {

constexpr size_t MaxDataBytes = 227;  // Matches PrivateApplication's admitted Data bound.
constexpr size_t MaxPayloadBytes = 233;  // Matches RouteMetadata::payload.
constexpr uint32_t MaxChannelSelector = 8;  // Stock MAX_NUM_CHANNELS; selector is one-based.
constexpr pb_size_t SessionPasskeyBytes = 8;

enum class Operation : uint8_t {
    Invalid = 0,
    Owner = meshtastic_AdminMessage_get_owner_request_tag,
    Config = meshtastic_AdminMessage_get_config_request_tag,
    ModuleConfig = meshtastic_AdminMessage_get_module_config_request_tag,
    Channel = meshtastic_AdminMessage_get_channel_request_tag,
    CannedMessages = meshtastic_AdminMessage_get_canned_message_module_messages_request_tag,
    Ringtone = meshtastic_AdminMessage_get_ringtone_request_tag,
    UiConfig = meshtastic_AdminMessage_get_ui_config_request_tag,
    DeviceMetadata = meshtastic_AdminMessage_get_device_metadata_request_tag,
    ConnectionStatus = meshtastic_AdminMessage_get_device_connection_status_request_tag,
    HardwarePins = meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag,
};

// Safe to retain as query metadata: contains no Admin response contents,
// passkey, peer identity, key, or authorization state.
struct Query {
    Operation operation = Operation::Invalid;
    uint8_t selector = 0;  // Config/module enum value or one-based channel selector.
};

struct Wipe {
    void *value;
    size_t size;
    ~Wipe() {
        volatile uint8_t *p = static_cast<volatile uint8_t *>(value);
        while (size--) *p++ = 0;
    }
};

// Nanopb may use short enum storage under -fshort-enums. Copy the object
// representation at its declared width so malformed wire values are checked
// without evaluating an out-of-range C++ enum value.
template <typename Enum>
inline bool enumNumber(const Enum &value, uint32_t &number) noexcept {
    static_assert(__is_enum(Enum), "Enum required");
    if constexpr (sizeof(Enum) == sizeof(uint8_t)) {
        uint8_t raw = 0;
        memcpy(&raw, &value, sizeof(raw));
        number = raw;
    } else if constexpr (sizeof(Enum) == sizeof(uint16_t)) {
        uint16_t raw = 0;
        memcpy(&raw, &value, sizeof(raw));
        number = raw;
    } else if constexpr (sizeof(Enum) == sizeof(uint32_t)) {
        memcpy(&number, &value, sizeof(number));
    } else {
        return false;
    }
    return true;
}

inline bool encodeCanonical(const meshtastic_Data &message, uint8_t *out,
                            size_t capacity, size_t &written) noexcept {
    auto stream = pb_ostream_from_buffer(out, capacity);
    if (!pb_encode(&stream, meshtastic_Data_fields, &message)) return false;
    written = stream.bytes_written;
    return true;
}

inline bool decodeCanonicalData(const uint8_t *wire, size_t length,
                                meshtastic_Data &message,
                                uint8_t (&canonical)[MaxDataBytes]) noexcept {
    if (!wire || length == 0 || length > MaxDataBytes) return false;
    auto input = pb_istream_from_buffer(wire, length);
    if (!pb_decode(&input, meshtastic_Data_fields, &message) || input.bytes_left != 0)
        return false;
    size_t written = 0;
    return encodeCanonical(message, canonical, sizeof(canonical), written) &&
           written == length && memcmp(canonical, wire, length) == 0;
}

inline bool decodeCanonicalAdmin(const uint8_t *wire, size_t length,
                                 meshtastic_AdminMessage &message,
                                 uint8_t (&canonical)[MaxPayloadBytes]) noexcept {
    if (!wire || length == 0 || length > MaxPayloadBytes) return false;
    auto input = pb_istream_from_buffer(wire, length);
    if (!pb_decode(&input, meshtastic_AdminMessage_fields, &message) || input.bytes_left != 0)
        return false;
    auto output = pb_ostream_from_buffer(canonical, sizeof(canonical));
    if (!pb_encode(&output, meshtastic_AdminMessage_fields, &message)) return false;
    return output.bytes_written == length &&
           memcmp(canonical, wire, length) == 0;
}

inline bool allowedConfigType(uint32_t value) noexcept {
    return value <= static_cast<uint32_t>(meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG);
}

inline bool allowedModuleConfigType(uint32_t value) noexcept {
    return value <= static_cast<uint32_t>(meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG) &&
           value != static_cast<uint32_t>(meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG);
}

// Read-only GET allowlist extension. Keep the ordinary module-type predicate
// restrictive so this inventory path cannot widen any mutation admission.
inline bool allowedModuleConfigGetType(uint32_t value) noexcept {
    return allowedModuleConfigType(value) ||
           value == static_cast<uint32_t>(meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG);
}

inline pb_size_t configResponseTag(uint8_t selector) noexcept {
    switch (selector) {
    case meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG: return meshtastic_Config_device_tag;
    case meshtastic_AdminMessage_ConfigType_POSITION_CONFIG: return meshtastic_Config_position_tag;
    case meshtastic_AdminMessage_ConfigType_POWER_CONFIG: return meshtastic_Config_power_tag;
    case meshtastic_AdminMessage_ConfigType_NETWORK_CONFIG: return meshtastic_Config_network_tag;
    case meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG: return meshtastic_Config_display_tag;
    case meshtastic_AdminMessage_ConfigType_LORA_CONFIG: return meshtastic_Config_lora_tag;
    case meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG: return meshtastic_Config_bluetooth_tag;
    case meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG: return meshtastic_Config_security_tag;
    case meshtastic_AdminMessage_ConfigType_SESSIONKEY_CONFIG: return meshtastic_Config_sessionkey_tag;
    case meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG: return meshtastic_Config_device_ui_tag;
    default: return 0;
    }
}

inline pb_size_t moduleConfigResponseTag(uint8_t selector) noexcept {
    switch (selector) {
    case meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG: return meshtastic_ModuleConfig_mqtt_tag;
    case meshtastic_AdminMessage_ModuleConfigType_SERIAL_CONFIG: return meshtastic_ModuleConfig_serial_tag;
    case meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG: return meshtastic_ModuleConfig_external_notification_tag;
    case meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG: return meshtastic_ModuleConfig_store_forward_tag;
    case meshtastic_AdminMessage_ModuleConfigType_RANGETEST_CONFIG: return meshtastic_ModuleConfig_range_test_tag;
    case meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG: return meshtastic_ModuleConfig_telemetry_tag;
    case meshtastic_AdminMessage_ModuleConfigType_CANNEDMSG_CONFIG: return meshtastic_ModuleConfig_canned_message_tag;
    case meshtastic_AdminMessage_ModuleConfigType_AUDIO_CONFIG: return meshtastic_ModuleConfig_audio_tag;
    case meshtastic_AdminMessage_ModuleConfigType_NEIGHBORINFO_CONFIG: return meshtastic_ModuleConfig_neighbor_info_tag;
    case meshtastic_AdminMessage_ModuleConfigType_AMBIENTLIGHTING_CONFIG: return meshtastic_ModuleConfig_ambient_lighting_tag;
    case meshtastic_AdminMessage_ModuleConfigType_DETECTIONSENSOR_CONFIG: return meshtastic_ModuleConfig_detection_sensor_tag;
    case meshtastic_AdminMessage_ModuleConfigType_PAXCOUNTER_CONFIG: return meshtastic_ModuleConfig_paxcounter_tag;
    case meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG: return meshtastic_ModuleConfig_statusmessage_tag;
    case meshtastic_AdminMessage_ModuleConfigType_TRAFFICMANAGEMENT_CONFIG: return meshtastic_ModuleConfig_traffic_management_tag;
    case meshtastic_AdminMessage_ModuleConfigType_TAK_CONFIG: return meshtastic_ModuleConfig_tak_tag;
    case meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG: return meshtastic_ModuleConfig_mesh_beacon_tag;
    case meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG: return meshtastic_ModuleConfig_remote_hardware_tag;
    default: return 0;
    }
}

inline bool validDataEnvelope(const meshtastic_Data &data, bool request) noexcept {
    uint32_t port = 0;
    if (!enumNumber(data.portnum, port) || port != meshtastic_PortNum_ADMIN_APP ||
        data.payload.size == 0 || data.payload.size > MaxPayloadBytes ||
        data.dest != 0 || data.source != 0 || data.reply_id != 0 || data.emoji != 0 ||
        data.xeddsa_signature.size != 0) return false;
    if (request) {
        // Stock Router sets Data.bitfield: bit 0 is config_ok_to_mqtt and bit
        // 1 is want_response. Older senders may use the legacy boolean alone.
        if (data.has_bitfield && (data.bitfield & ~uint8_t{0x03}) != 0) return false;
        const bool wantsResponse = data.want_response ||
            (data.has_bitfield && (data.bitfield & uint8_t{0x02}) != 0);
        return wantsResponse && data.request_id == 0;
    }
    // Stock responses carry the bitfield too; bit 1 must remain clear so the
    // receiver does not reinterpret the response as another request.
    return data.has_bitfield && (data.bitfield & ~uint8_t{0x01}) == 0 &&
           !data.want_response && data.request_id != 0;
}

inline bool extractCanonicalPayload(const uint8_t *wire, size_t length,
                                    bool request, uint32_t requestPacketId,
                                    uint8_t (&payload)[MaxPayloadBytes],
                                    size_t &payloadSize) noexcept {
    meshtastic_Data data{};
    Wipe wipeData{&data, sizeof(data)};
    uint8_t canonical[MaxDataBytes]{};
    Wipe wipeCanonical{canonical, sizeof(canonical)};
    if (!decodeCanonicalData(wire, length, data, canonical) ||
        !validDataEnvelope(data, request) ||
        (!request && (requestPacketId == 0 || data.request_id != requestPacketId))) return false;
    payloadSize = data.payload.size;
    memcpy(payload, data.payload.bytes, payloadSize);
    return true;
}

inline bool validQuery(const Query &query) noexcept {
    switch (query.operation) {
    case Operation::Owner: return query.selector == 0;
    case Operation::Config: return allowedConfigType(query.selector);
    case Operation::ModuleConfig: return allowedModuleConfigGetType(query.selector);
    case Operation::Channel: return query.selector >= 1 && query.selector <= MaxChannelSelector;
    case Operation::CannedMessages:
    case Operation::Ringtone:
    case Operation::UiConfig:
    case Operation::DeviceMetadata:
    case Operation::ConnectionStatus:
    case Operation::HardwarePins: return query.selector == 0;
    default: return false;
    }
}

// This validates plaintext wire shape only. Authentication, unicast/PKI
// verification, and admission policy belong to the caller.
inline bool canonicalGetRequest(const uint8_t *wire, size_t length,
                                Query &result) noexcept {
    uint8_t payload[MaxPayloadBytes]{};
    Wipe wipePayload{payload, sizeof(payload)};
    size_t payloadSize = 0;
    if (!extractCanonicalPayload(wire, length, true, 0, payload, payloadSize)) return false;

    meshtastic_AdminMessage admin{};
    Wipe wipeAdmin{&admin, sizeof(admin)};
    uint8_t canonicalAdmin[MaxPayloadBytes]{};
    Wipe wipeCanonicalAdmin{canonicalAdmin, sizeof(canonicalAdmin)};
    if (!decodeCanonicalAdmin(payload, payloadSize, admin, canonicalAdmin)) return false;
    // Stock clients cache and attach the eight-byte session passkey to later
    // GETs. It is accepted only as an opaque wire field; this validator never
    // retains, logs, compares, or uses it as authorization.
    if (admin.session_passkey.size != 0 &&
        admin.session_passkey.size != SessionPasskeyBytes) return false;

    Query candidate{};
    switch (admin.which_payload_variant) {
    case meshtastic_AdminMessage_get_owner_request_tag:
        if (!admin.get_owner_request) return false;
        candidate.operation = Operation::Owner;
        break;
    case meshtastic_AdminMessage_get_config_request_tag: {
        uint32_t value = 0;
        if (!enumNumber(admin.get_config_request, value)) return false;
        if (!allowedConfigType(value)) return false;
        candidate.operation = Operation::Config;
        candidate.selector = static_cast<uint8_t>(value);
        break;
    }
    case meshtastic_AdminMessage_get_module_config_request_tag: {
        uint32_t value = 0;
        if (!enumNumber(admin.get_module_config_request, value)) return false;
        if (!allowedModuleConfigGetType(value)) return false;
        candidate.operation = Operation::ModuleConfig;
        candidate.selector = static_cast<uint8_t>(value);
        break;
    }
    case meshtastic_AdminMessage_get_channel_request_tag:
        if (admin.get_channel_request < 1 ||
            admin.get_channel_request > MaxChannelSelector) return false;
        candidate.operation = Operation::Channel;
        candidate.selector = static_cast<uint8_t>(admin.get_channel_request);
        break;
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        if(!admin.get_canned_message_module_messages_request)return false;
        candidate.operation=Operation::CannedMessages;break;
    case meshtastic_AdminMessage_get_ringtone_request_tag:
        if(!admin.get_ringtone_request)return false;
        candidate.operation=Operation::Ringtone;break;
    case meshtastic_AdminMessage_get_ui_config_request_tag:
        if(!admin.get_ui_config_request)return false;
        candidate.operation=Operation::UiConfig;break;
    case meshtastic_AdminMessage_get_device_metadata_request_tag:
        if(!admin.get_device_metadata_request)return false;
        candidate.operation=Operation::DeviceMetadata;break;
    case meshtastic_AdminMessage_get_device_connection_status_request_tag:
        if(!admin.get_device_connection_status_request)return false;
        candidate.operation=Operation::ConnectionStatus;break;
    case meshtastic_AdminMessage_get_node_remote_hardware_pins_request_tag:
        if(!admin.get_node_remote_hardware_pins_request)return false;
        candidate.operation=Operation::HardwarePins;break;
    default:
        return false;
    }
    if (!validQuery(candidate)) return false;
    result = candidate;
    return true;
}

// Checks a response against the originating query without retaining response
// bytes. The caller remains responsible for peer/key authorization, pending
// query lifetime, encryption, redaction, and one-time correlation state.
inline bool canonicalGetResponse(const uint8_t *wire, size_t length,
                                 const Query &query,
                                 uint32_t requestPacketId) noexcept {
    if (!validQuery(query) || requestPacketId == 0) return false;
    uint8_t payload[MaxPayloadBytes]{};
    Wipe wipePayload{payload, sizeof(payload)};
    size_t payloadSize = 0;
    if (!extractCanonicalPayload(wire, length, false, requestPacketId,
                                 payload, payloadSize)) return false;

    meshtastic_AdminMessage admin{};
    Wipe wipeAdmin{&admin, sizeof(admin)};
    uint8_t canonicalAdmin[MaxPayloadBytes]{};
    Wipe wipeCanonicalAdmin{canonicalAdmin, sizeof(canonicalAdmin)};
    if (!decodeCanonicalAdmin(payload, payloadSize, admin, canonicalAdmin) ||
        admin.session_passkey.size != SessionPasskeyBytes) return false;

    if(admin.which_payload_variant!=stock::admin::responseForRequest(static_cast<pb_size_t>(query.operation)))
        return false;
    switch(query.operation) {
    case Operation::Config:
        return admin.get_config_response.which_payload_variant==configResponseTag(query.selector);
    case Operation::ModuleConfig:
        return admin.get_module_config_response.which_payload_variant==moduleConfigResponseTag(query.selector);
    case Operation::Channel:
        return admin.get_channel_response.index==int32_t(query.selector-1);
    default:return true; // validQuery and responseForRequest have already matched this exact oneof.
    }
}

}  // namespace mt::admin::remote_protocol
