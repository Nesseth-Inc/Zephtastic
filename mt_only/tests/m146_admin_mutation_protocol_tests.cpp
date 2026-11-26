#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <vector>

#include "admin/RemoteAdminMutationProtocol.h"

using mt::admin::mutation_protocol::Mutation;
using mt::admin::mutation_protocol::Operation;
using mt::admin::mutation_protocol::canonicalSetRequest;

namespace {
struct Wire { std::array<uint8_t, 512> bytes{}; size_t size = 0; };

Wire encodeAdmin(const meshtastic_AdminMessage &admin) {
    Wire wire;
    auto stream = pb_ostream_from_buffer(wire.bytes.data(), wire.bytes.size());
    assert(pb_encode(&stream, meshtastic_AdminMessage_fields, &admin));
    wire.size = stream.bytes_written;
    return wire;
}

Wire encodeData(const uint8_t *payload, size_t payloadSize,
                bool wantResponse = true, bool hasBitfield = true,
                uint8_t bitfield = 2, uint32_t requestId = 0,
                meshtastic_PortNum port = meshtastic_PortNum_ADMIN_APP,
                uint32_t dest = 0) {
    meshtastic_Data data{};
    data.portnum = port;
    assert(payloadSize <= sizeof(data.payload.bytes));
    data.payload.size = static_cast<pb_size_t>(payloadSize);
    memcpy(data.payload.bytes, payload, payloadSize);
    data.want_response = wantResponse;
    data.has_bitfield = hasBitfield;
    data.bitfield = bitfield;
    data.request_id = requestId;
    data.dest = dest;
    Wire wire;
    auto stream = pb_ostream_from_buffer(wire.bytes.data(), wire.bytes.size());
    assert(pb_encode(&stream, meshtastic_Data_fields, &data));
    wire.size = stream.bytes_written;
    return wire;
}

Wire encodeRequest(const meshtastic_AdminMessage &admin,
                   bool wantResponse = true, bool hasBitfield = true,
                   uint8_t bitfield = 2, uint32_t requestId = 0,
                   meshtastic_PortNum port = meshtastic_PortNum_ADMIN_APP,
                   uint32_t dest = 0) {
    const Wire payload = encodeAdmin(admin);
    return encodeData(payload.bytes.data(), payload.size, wantResponse,
                      hasBitfield, bitfield, requestId, port, dest);
}

meshtastic_AdminMessage baseSet() {
    meshtastic_AdminMessage admin{};
    admin.session_passkey.size = 8;
    for (unsigned i = 0; i < 8; ++i) admin.session_passkey.bytes[i] = uint8_t(0x40 + i);
    return admin;
}

void assertRejectedWithoutChanging(const Wire &wire) {
    Mutation out{Operation::ModuleConfig, 0xEE};
    assert(!canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::ModuleConfig && out.selector == 0xEE);
}

void testAllowedSetShapesAndSelectors() {
    auto admin = baseSet();
    admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
    auto wire = encodeRequest(admin);
    Mutation out{};
    assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::Owner && out.selector == 0);

    admin = baseSet();
    admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    admin.set_config.which_payload_variant = meshtastic_Config_device_tag;
    wire = encodeRequest(admin);
    assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::Config && out.selector == meshtastic_Config_device_tag);

    admin = baseSet();
    admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    admin.set_config.which_payload_variant = meshtastic_Config_network_tag;
    wire = encodeRequest(admin);
    assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::Config && out.selector == meshtastic_Config_network_tag);

    admin = baseSet();
    admin.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
    admin.set_module_config.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
    wire = encodeRequest(admin);
    assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::ModuleConfig && out.selector == meshtastic_ModuleConfig_mqtt_tag);

    for (int32_t index = 0; index <= 7; ++index) {
        admin = baseSet();
        admin.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
        admin.set_channel.index = index;
        wire = encodeRequest(admin);
        assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
        assert(out.operation == Operation::Channel && out.selector == index);
    }
}

void testExcludedConfigAndModuleVariants() {
    for (pb_size_t tag : {meshtastic_Config_position_tag, meshtastic_Config_power_tag,
                          meshtastic_Config_display_tag, meshtastic_Config_lora_tag,
                          meshtastic_Config_bluetooth_tag, meshtastic_Config_security_tag,
                          meshtastic_Config_sessionkey_tag, meshtastic_Config_device_ui_tag}) {
        auto admin = baseSet();
        admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
        admin.set_config.which_payload_variant = tag;
        assertRejectedWithoutChanging(encodeRequest(admin));
    }
    for (pb_size_t tag : {meshtastic_ModuleConfig_serial_tag,
                          meshtastic_ModuleConfig_external_notification_tag,
                          meshtastic_ModuleConfig_remote_hardware_tag,
                          meshtastic_ModuleConfig_telemetry_tag}) {
        auto admin = baseSet();
        admin.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
        admin.set_module_config.which_payload_variant = tag;
        assertRejectedWithoutChanging(encodeRequest(admin));
    }
}

void testOtherAdminOperationsAndInvalidChannels() {
    for (pb_size_t operation : {meshtastic_AdminMessage_get_owner_request_tag,
                                meshtastic_AdminMessage_get_config_request_tag,
                                meshtastic_AdminMessage_get_owner_response_tag,
                                meshtastic_AdminMessage_set_ham_mode_tag,
                                meshtastic_AdminMessage_factory_reset_config_tag,
                                meshtastic_AdminMessage_set_favorite_node_tag}) {
        auto admin = baseSet();
        admin.which_payload_variant = operation;
        assertRejectedWithoutChanging(encodeRequest(admin));
    }
    for (int32_t index : {-1, 8, 9}) {
        auto admin = baseSet();
        admin.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
        admin.set_channel.index = index;
        assertRejectedWithoutChanging(encodeRequest(admin));
    }
}

void testEightBytePasskeyWidthOnly() {
    Mutation out{};
    auto admin = baseSet();
    admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
    for (pb_size_t length : {pb_size_t(0), pb_size_t(7)}) {
        admin.session_passkey.size = length;
        assertRejectedWithoutChanging(encodeRequest(admin));
    }
    admin.session_passkey.size = 8;
    auto accepted = encodeRequest(admin);
    assert(canonicalSetRequest(accepted.bytes.data(), accepted.size, out));

    // A ninth passkey byte cannot fit the pinned bytes-array type. Construct
    // the malformed wire field directly: Admin field 101, length-delimited.
    Wire payload = encodeAdmin(admin);
    assert(payload.size + 11 < payload.bytes.size());
    payload.bytes[payload.size++] = 0xAA;  // key 810: field 101, wire type 2.
    payload.bytes[payload.size++] = 0x06;
    payload.bytes[payload.size++] = 9;
    for (unsigned i = 0; i < 9; ++i) payload.bytes[payload.size++] = uint8_t(i);
    assertRejectedWithoutChanging(encodeData(payload.bytes.data(), payload.size));
}

void testRequestEnvelopeSemanticsAndCanonicality() {
    auto admin = baseSet();
    admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
    Mutation out{};
    auto stock = encodeRequest(admin, true, true, 2);
    assert(canonicalSetRequest(stock.bytes.data(), stock.size, out));
    auto legacyBoolean = encodeRequest(admin, true, false, 0);
    assert(canonicalSetRequest(legacyBoolean.bytes.data(), legacyBoolean.size, out));
    auto bitfieldOnly = encodeRequest(admin, false, true, 2);
    assert(canonicalSetRequest(bitfieldOnly.bytes.data(), bitfieldOnly.size, out));

    assertRejectedWithoutChanging(encodeRequest(admin, false, false, 0));
    assertRejectedWithoutChanging(encodeRequest(admin, true, true, 4));
    assertRejectedWithoutChanging(encodeRequest(admin, true, true, 2, 123));
    assertRejectedWithoutChanging(encodeRequest(admin, true, true, 2, 0,
                                                meshtastic_PortNum_TEXT_MESSAGE_APP));
    assertRejectedWithoutChanging(encodeRequest(admin, true, true, 2, 0,
                                                meshtastic_PortNum_ADMIN_APP, 7));

    Wire payload = encodeAdmin(admin);
    assert(payload.size + 3 < payload.bytes.size());
    payload.bytes[payload.size++] = 0xF0;  // Unknown Admin field 126, varint.
    payload.bytes[payload.size++] = 0x07;
    payload.bytes[payload.size++] = 1;
    assertRejectedWithoutChanging(encodeData(payload.bytes.data(), payload.size));

    Wire badData = stock;
    assert(badData.size + 2 < badData.bytes.size());
    badData.bytes[badData.size++] = 0x78;  // Unknown Data field 15, varint.
    badData.bytes[badData.size++] = 1;
    assertRejectedWithoutChanging(badData);
}
}  // namespace

int main() {
    testAllowedSetShapesAndSelectors();
    testExcludedConfigAndModuleVariants();
    testOtherAdminOperationsAndInvalidChannels();
    testEightBytePasskeyWidthOnly();
    testRequestEnvelopeSemanticsAndCanonicality();
}
