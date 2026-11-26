#include "RemoteAdminProtocol.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace mt::admin::remote_protocol;

namespace {

struct Wire {
    std::array<uint8_t, MaxDataBytes> bytes{};
    std::size_t size = 0;
};

struct Payload {
    std::array<uint8_t, MaxPayloadBytes> bytes{};
    std::size_t size = 0;
};

Payload encodeAdmin(const meshtastic_AdminMessage &message) {
    Payload result{};
    auto stream = pb_ostream_from_buffer(result.bytes.data(), result.bytes.size());
    assert(pb_encode(&stream, meshtastic_AdminMessage_fields, &message));
    assert(stream.bytes_written > 0 && stream.bytes_written <= MaxPayloadBytes);
    result.size = stream.bytes_written;
    return result;
}

Wire encodeData(const uint8_t *payload, std::size_t payloadSize,
                bool wantResponse, uint32_t requestId = 0,
                meshtastic_PortNum port = meshtastic_PortNum_ADMIN_APP,
                bool withSignature = false, uint32_t source = 0,
                bool withBitfield = true, uint8_t bitfield = 0xFF) {
    assert(payload && payloadSize > 0 && payloadSize <= MaxPayloadBytes);
    meshtastic_Data data{};
    data.portnum = port;
    data.payload.size = static_cast<pb_size_t>(payloadSize);
    std::memcpy(data.payload.bytes, payload, payloadSize);
    data.want_response = wantResponse;
    data.has_bitfield = withBitfield;
    data.bitfield = bitfield == 0xFF ? static_cast<uint8_t>(wantResponse ? 2 : 0) : bitfield;
    data.request_id = requestId;
    data.source = source;
    if (withSignature) {
        data.xeddsa_signature.size = 1;
        data.xeddsa_signature.bytes[0] = 0xA5;
    }
    Wire result{};
    auto stream = pb_ostream_from_buffer(result.bytes.data(), result.bytes.size());
    assert(pb_encode(&stream, meshtastic_Data_fields, &data));
    result.size = stream.bytes_written;
    return result;
}

Wire requestWire(meshtastic_AdminMessage &admin, bool wantResponse = true,
                 uint32_t dataRequestId = 0,
                 meshtastic_PortNum port = meshtastic_PortNum_ADMIN_APP,
                 bool withSignature = false, uint32_t source = 0,
                 bool withBitfield = true, uint8_t bitfield = 0xFF) {
    const auto payload = encodeAdmin(admin);
    return encodeData(payload.bytes.data(), payload.size, wantResponse,
                      dataRequestId, port, withSignature, source, withBitfield, bitfield);
}

Wire responseWire(meshtastic_AdminMessage &admin, uint32_t requestId) {
    const auto payload = encodeAdmin(admin);
    return encodeData(payload.bytes.data(), payload.size, false, requestId);
}

meshtastic_AdminMessage ownerRequest() {
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant = meshtastic_AdminMessage_get_owner_request_tag;
    admin.get_owner_request = true;
    return admin;
}

meshtastic_AdminMessage ownerResponse() {
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
    admin.session_passkey.size = SessionPasskeyBytes;
    for (pb_size_t i = 0; i < admin.session_passkey.size; ++i)
        admin.session_passkey.bytes[i] = static_cast<pb_byte_t>(0xA0 + i);
    return admin;
}

pb_size_t configTag(uint8_t selector) {
    static constexpr pb_size_t tags[] = {
        meshtastic_Config_device_tag, meshtastic_Config_position_tag,
        meshtastic_Config_power_tag, meshtastic_Config_network_tag,
        meshtastic_Config_display_tag, meshtastic_Config_lora_tag,
        meshtastic_Config_bluetooth_tag, meshtastic_Config_security_tag,
        meshtastic_Config_sessionkey_tag, meshtastic_Config_device_ui_tag};
    assert(selector < sizeof(tags) / sizeof(tags[0]));
    return tags[selector];
}

pb_size_t moduleTag(uint8_t selector) {
    static constexpr pb_size_t tags[] = {
        meshtastic_ModuleConfig_mqtt_tag, meshtastic_ModuleConfig_serial_tag,
        meshtastic_ModuleConfig_external_notification_tag,
        meshtastic_ModuleConfig_store_forward_tag, meshtastic_ModuleConfig_range_test_tag,
        meshtastic_ModuleConfig_telemetry_tag, meshtastic_ModuleConfig_canned_message_tag,
        meshtastic_ModuleConfig_audio_tag, meshtastic_ModuleConfig_remote_hardware_tag,
        meshtastic_ModuleConfig_neighbor_info_tag, meshtastic_ModuleConfig_ambient_lighting_tag,
        meshtastic_ModuleConfig_detection_sensor_tag, meshtastic_ModuleConfig_paxcounter_tag,
        meshtastic_ModuleConfig_statusmessage_tag, meshtastic_ModuleConfig_traffic_management_tag,
        meshtastic_ModuleConfig_tak_tag, meshtastic_ModuleConfig_mesh_beacon_tag};
    assert(selector < sizeof(tags) / sizeof(tags[0]));
    return tags[selector];
}

meshtastic_AdminMessage configRequest(uint8_t selector) {
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant = meshtastic_AdminMessage_get_config_request_tag;
    admin.get_config_request = static_cast<meshtastic_AdminMessage_ConfigType>(selector);
    return admin;
}

meshtastic_AdminMessage moduleRequest(uint8_t selector) {
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant = meshtastic_AdminMessage_get_module_config_request_tag;
    admin.get_module_config_request = static_cast<meshtastic_AdminMessage_ModuleConfigType>(selector);
    return admin;
}

meshtastic_AdminMessage configResponse(uint8_t selector) {
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
    admin.get_config_response.which_payload_variant = configTag(selector);
    admin.session_passkey.size = SessionPasskeyBytes;
    std::memset(admin.session_passkey.bytes, 0xB7, admin.session_passkey.size);
    return admin;
}

meshtastic_AdminMessage moduleResponse(uint8_t selector) {
    meshtastic_AdminMessage admin{};
    admin.which_payload_variant = meshtastic_AdminMessage_get_module_config_response_tag;
    admin.get_module_config_response.which_payload_variant = moduleTag(selector);
    admin.session_passkey.size = SessionPasskeyBytes;
    std::memset(admin.session_passkey.bytes, 0xC8, admin.session_passkey.size);
    return admin;
}

void testOwnerQueryAndExactResponseCorrelation() {
    constexpr uint32_t packetId = 0x10203040;
    auto req = ownerRequest();
    auto request = requestWire(req);
    Query query{};
    assert(canonicalGetRequest(request.bytes.data(), request.size, query));
    assert(query.operation == Operation::Owner && query.selector == 0);

    auto response = ownerResponse();
    auto reply = responseWire(response, packetId);
    assert(canonicalGetResponse(reply.bytes.data(), reply.size, query, packetId));
    auto wrongId = responseWire(response, packetId + 1);
    assert(!canonicalGetResponse(wrongId.bytes.data(), wrongId.size, query, packetId));
    assert(!canonicalGetResponse(reply.bytes.data(), reply.size, query, 0));
}

void testEverySupportedConfigEnumAndResponseType() {
    constexpr uint32_t packetId = 0x10203041;
    for (uint8_t type = 0; type <= meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG; ++type) {
        auto req = configRequest(type);
        auto request = requestWire(req);
        Query query{};
        assert(canonicalGetRequest(request.bytes.data(), request.size, query));
        assert(query.operation == Operation::Config && query.selector == type);

        auto response = configResponse(type);
        auto reply = responseWire(response, packetId + type);
        assert(canonicalGetResponse(reply.bytes.data(), reply.size, query, packetId + type));

        response.get_config_response.which_payload_variant = configTag((type + 1) % 10);
        auto mismatched = responseWire(response, packetId + type);
        assert(!canonicalGetResponse(mismatched.bytes.data(), mismatched.size, query, packetId + type));
    }
}

void testEverySupportedModuleEnumAndRemoteHardwareExclusion() {
    constexpr uint32_t packetId = 0x10203100;
    for (uint8_t type = 0; type <= meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG; ++type) {
        auto req = moduleRequest(type);
        auto request = requestWire(req);
        Query query{};
        const bool accepted = canonicalGetRequest(request.bytes.data(), request.size, query);
        if (type == meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG) {
            assert(!accepted);
            continue;
        }
        assert(accepted);
        assert(query.operation == Operation::ModuleConfig && query.selector == type);

        auto response = moduleResponse(type);
        auto reply = responseWire(response, packetId + type);
        assert(canonicalGetResponse(reply.bytes.data(), reply.size, query, packetId + type));

        response.get_module_config_response.which_payload_variant = moduleTag((type + 1) % 17);
        auto mismatched = responseWire(response, packetId + type);
        assert(!canonicalGetResponse(mismatched.bytes.data(), mismatched.size, query, packetId + type));
    }
}

void testOneBasedChannelSelectorAndMatchingIndex() {
    constexpr uint32_t packetId = 0x10203200;
    for (uint32_t selector = 1; selector <= MaxChannelSelector; ++selector) {
        meshtastic_AdminMessage req{};
        req.which_payload_variant = meshtastic_AdminMessage_get_channel_request_tag;
        req.get_channel_request = selector;
        auto request = requestWire(req);
        Query query{};
        assert(canonicalGetRequest(request.bytes.data(), request.size, query));
        assert(query.operation == Operation::Channel && query.selector == selector);

        meshtastic_AdminMessage response{};
        response.which_payload_variant = meshtastic_AdminMessage_get_channel_response_tag;
        response.get_channel_response.index = static_cast<int32_t>(selector - 1);
        response.session_passkey.size = SessionPasskeyBytes;
        std::memset(response.session_passkey.bytes, 0xD9, response.session_passkey.size);
        auto reply = responseWire(response, packetId + selector);
        assert(canonicalGetResponse(reply.bytes.data(), reply.size, query, packetId + selector));
        response.get_channel_response.index = static_cast<int32_t>(selector % MaxChannelSelector);
        auto mismatched = responseWire(response, packetId + selector);
        assert(!canonicalGetResponse(mismatched.bytes.data(), mismatched.size, query, packetId + selector));
    }
}

void testUnsupportedSelectorsAndResponseMetadataRefused() {
    Query query{};
    const uint8_t invalidConfigEnum[] = {0x28, 0x0A};  // get_config_request = 10.
    auto configWire = encodeData(invalidConfigEnum, sizeof(invalidConfigEnum), true);
    assert(!canonicalGetRequest(configWire.bytes.data(), configWire.size, query));
    const uint8_t largeConfigEnum[] = {0x28, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    auto largeConfigWire = encodeData(largeConfigEnum, sizeof(largeConfigEnum), true);
    assert(!canonicalGetRequest(largeConfigWire.bytes.data(), largeConfigWire.size, query));

    const uint8_t invalidModuleEnum[] = {0x30, 0x11};  // get_module_config_request = 17.
    auto moduleWire = encodeData(invalidModuleEnum, sizeof(invalidModuleEnum), true);
    assert(!canonicalGetRequest(moduleWire.bytes.data(), moduleWire.size, query));

    meshtastic_AdminMessage channel{};
    channel.which_payload_variant = meshtastic_AdminMessage_get_channel_request_tag;
    channel.get_channel_request = 0;
    auto zeroChannel = requestWire(channel);
    assert(!canonicalGetRequest(zeroChannel.bytes.data(), zeroChannel.size, query));
    channel.get_channel_request = MaxChannelSelector + 1;
    auto highChannel = requestWire(channel);
    assert(!canonicalGetRequest(highChannel.bytes.data(), highChannel.size, query));
    auto wrongPasskeySize = ownerResponse();
    wrongPasskeySize.session_passkey.size = SessionPasskeyBytes - 1;
    auto shortPasskey = responseWire(wrongPasskeySize, 14);
    assert(!canonicalGetResponse(shortPasskey.bytes.data(), shortPasskey.size,
                                 Query{Operation::Owner, 0}, 14));

    auto requestAsReply = ownerResponse();
    auto badResponseEnvelope = requestWire(requestAsReply, true, 14);
    assert(!canonicalGetResponse(badResponseEnvelope.bytes.data(), badResponseEnvelope.size,
                                 Query{Operation::Owner, 0}, 14));
    auto validOwnerReply = ownerResponse();
    auto validOwnerReplyWire = responseWire(validOwnerReply, 15);
    assert(!canonicalGetResponse(validOwnerReplyWire.bytes.data(), validOwnerReplyWire.size,
                                 Query{Operation::Channel, MaxChannelSelector + 1}, 15));
    const auto replyPayload = encodeAdmin(validOwnerReply);
    auto invalidReplyBits = encodeData(replyPayload.bytes.data(), replyPayload.size, false, 15,
                                       meshtastic_PortNum_ADMIN_APP, false, 0, true, 2);
    assert(!canonicalGetResponse(invalidReplyBits.bytes.data(), invalidReplyBits.size,
                                 Query{Operation::Owner, 0}, 15));

    std::array<uint8_t, MaxDataBytes + 1> oversized{};
    assert(!canonicalGetRequest(oversized.data(), oversized.size(), query));
    assert(!canonicalGetRequest(nullptr, 0, query));
}

void testDataMetadataAndOneofPolicy() {
    auto req = ownerRequest();
    Query query{};
    auto noResponseFlag = requestWire(req, false, 0, meshtastic_PortNum_ADMIN_APP,
                                      false, 0, true, 0);
    assert(!canonicalGetRequest(noResponseFlag.bytes.data(), noResponseFlag.size, query));
    auto requestIdOnQuery = requestWire(req, true, 21);
    assert(!canonicalGetRequest(requestIdOnQuery.bytes.data(), requestIdOnQuery.size, query));
    auto explicitZeroRequestId = requestWire(req);
    assert(explicitZeroRequestId.size + 5 <= explicitZeroRequestId.bytes.size());
    explicitZeroRequestId.bytes[explicitZeroRequestId.size++] = 0x35;  // Data.request_id, fixed32.
    for (unsigned i = 0; i < 4; ++i)
        explicitZeroRequestId.bytes[explicitZeroRequestId.size++] = 0;
    assert(!canonicalGetRequest(explicitZeroRequestId.bytes.data(),
                                explicitZeroRequestId.size, query));
    auto wrongPort = requestWire(req, true, 0, static_cast<meshtastic_PortNum>(1));
    assert(!canonicalGetRequest(wrongPort.bytes.data(), wrongPort.size, query));
    auto signedData = requestWire(req, true, 0, meshtastic_PortNum_ADMIN_APP, true);
    assert(!canonicalGetRequest(signedData.bytes.data(), signedData.size, query));
    auto unrelatedMetadata = requestWire(req, true, 0, meshtastic_PortNum_ADMIN_APP, false, 0x1234);
    assert(!canonicalGetRequest(unrelatedMetadata.bytes.data(), unrelatedMetadata.size, query));
    auto legacyBoolean = requestWire(req, true, 0, meshtastic_PortNum_ADMIN_APP,
                                     false, 0, false);
    assert(canonicalGetRequest(legacyBoolean.bytes.data(), legacyBoolean.size, query));
    auto bitfieldOnlyRequest = requestWire(req, false, 0, meshtastic_PortNum_ADMIN_APP,
                                           false, 0, true, 2);
    assert(canonicalGetRequest(bitfieldOnlyRequest.bytes.data(), bitfieldOnlyRequest.size, query));
    auto invalidRequestBits = requestWire(req, true, 0, meshtastic_PortNum_ADMIN_APP,
                                          false, 0, true, 4);
    assert(!canonicalGetRequest(invalidRequestBits.bytes.data(), invalidRequestBits.size, query));
    auto largePort = requestWire(req);
    assert(largePort.bytes[0] == 0x08 && largePort.bytes[1] == meshtastic_PortNum_ADMIN_APP);
    std::memmove(largePort.bytes.data() + 6, largePort.bytes.data() + 2, largePort.size - 2);
    largePort.bytes[1] = 0xFF; largePort.bytes[2] = 0xFF; largePort.bytes[3] = 0xFF;
    largePort.bytes[4] = 0xFF; largePort.bytes[5] = 0x0F;
    largePort.size += 4;
    assert(!canonicalGetRequest(largePort.bytes.data(), largePort.size, query));

    req.get_owner_request = false;
    auto falseOwner = requestWire(req);
    assert(!canonicalGetRequest(falseOwner.bytes.data(), falseOwner.size, query));
    req = ownerRequest();
    req.session_passkey.size = SessionPasskeyBytes - 1;
    auto shortPasskeyOnRequest = requestWire(req);
    assert(!canonicalGetRequest(shortPasskeyOnRequest.bytes.data(), shortPasskeyOnRequest.size, query));
    req.session_passkey.size = SessionPasskeyBytes;
    std::memset(req.session_passkey.bytes, 0x5A, SessionPasskeyBytes);
    auto cachedPasskeyOnRequest = requestWire(req);
    assert(canonicalGetRequest(cachedPasskeyOnRequest.bytes.data(), cachedPasskeyOnRequest.size, query));

    meshtastic_AdminMessage mutation{};
    mutation.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
    auto setter = requestWire(mutation);
    assert(!canonicalGetRequest(setter.bytes.data(), setter.size, query));
}

void testUnknownAndDuplicateWireFieldsFailCanonicalComparison() {
    auto req = ownerRequest();
    auto valid = requestWire(req);
    assert(valid.size + 2 <= valid.bytes.size());
    valid.bytes[valid.size++] = 0x78;  // Unknown Data field 15, varint wire type.
    valid.bytes[valid.size++] = 0x01;
    Query query{};
    assert(!canonicalGetRequest(valid.bytes.data(), valid.size, query));

    valid = requestWire(req);
    assert(valid.size + 2 <= valid.bytes.size());
    valid.bytes[valid.size++] = 0x08;  // Duplicate portnum field.
    valid.bytes[valid.size++] = meshtastic_PortNum_ADMIN_APP;
    assert(!canonicalGetRequest(valid.bytes.data(), valid.size, query));

    auto payload = encodeAdmin(req);
    assert(payload.size + 2 <= payload.bytes.size());
    payload.bytes[payload.size++] = 0x18;  // Duplicate get_owner_request oneof field.
    payload.bytes[payload.size++] = 0x01;
    auto duplicateAdmin = encodeData(payload.bytes.data(), payload.size, true);
    assert(!canonicalGetRequest(duplicateAdmin.bytes.data(), duplicateAdmin.size, query));
}

}  // namespace

int main() {
    testOwnerQueryAndExactResponseCorrelation();
    testEverySupportedConfigEnumAndResponseType();
    testEverySupportedModuleEnumAndRemoteHardwareExclusion();
    testOneBasedChannelSelectorAndMatchingIndex();
    testUnsupportedSelectorsAndResponseMetadataRefused();
    testDataMetadataAndOneofPolicy();
    testUnknownAndDuplicateWireFieldsFailCanonicalComparison();
}
