#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <initializer_list>

#include <array>

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
                bool wantResponse = true, uint32_t requestId = 0,
                meshtastic_PortNum port = meshtastic_PortNum_ADMIN_APP,
                bool hasBitfield = true, uint8_t bitfield = 2) {
    meshtastic_Data data{};
    data.portnum = port;
    assert(payloadSize <= sizeof(data.payload.bytes));
    data.payload.size = static_cast<pb_size_t>(payloadSize);
    memcpy(data.payload.bytes, payload, payloadSize);
    data.want_response = wantResponse;
    data.has_bitfield = hasBitfield;
    data.bitfield = bitfield;
    data.request_id = requestId;
    Wire wire;
    auto stream = pb_ostream_from_buffer(wire.bytes.data(), wire.bytes.size());
    assert(pb_encode(&stream, meshtastic_Data_fields, &data));
    wire.size = stream.bytes_written;
    return wire;
}

Wire encodeRequest(const meshtastic_AdminMessage &admin,
                   bool wantResponse = true, uint32_t requestId = 0,
                   meshtastic_PortNum port = meshtastic_PortNum_ADMIN_APP,
                   bool hasBitfield = true, uint8_t bitfield = 2) {
    const Wire payload = encodeAdmin(admin);
    return encodeData(payload.bytes.data(), payload.size, wantResponse, requestId, port,
                      hasBitfield, bitfield);
}

meshtastic_AdminMessage baseAdmin() {
    meshtastic_AdminMessage admin{};
    admin.session_passkey.size = 8;
    for (unsigned i = 0; i < 8; ++i) admin.session_passkey.bytes[i] = uint8_t(0xA0 + i);
    return admin;
}

void appendVarint(Wire &wire, uint32_t value) {
    while (value >= 0x80u) {
        assert(wire.size < wire.bytes.size());
        wire.bytes[wire.size++] = static_cast<uint8_t>(value) | 0x80u;
        value >>= 7;
    }
    assert(wire.size < wire.bytes.size());
    wire.bytes[wire.size++] = static_cast<uint8_t>(value);
}

void assertRejectedUnchanged(const Wire &wire) {
    Mutation out{Operation::ModuleConfig, 0xEE, 0x89abcdefu};
    assert(!canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::ModuleConfig && out.selector == 0xEE &&
           out.node == 0x89abcdefu);
}

Wire favoriteRequest(uint32_t node, bool setFavorite) {
    auto admin = baseAdmin();
    if (setFavorite) {
        admin.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
        admin.set_favorite_node = node;
    } else {
        admin.which_payload_variant = meshtastic_AdminMessage_remove_favorite_node_tag;
        admin.remove_favorite_node = node;
    }
    return encodeRequest(admin);
}

void testBothFavoriteActionsKeepFullNodeNumber() {
    constexpr uint32_t node = 0x12345678u;  // Catches accidental uint8_t selector narrowing.
    for (bool setFavorite : {true, false}) {
        Wire wire = favoriteRequest(node, setFavorite);
        const auto original = wire.bytes;
        Mutation out{};
        assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
        assert(out.operation == (setFavorite ? Operation::Favorite : Operation::Unfavorite));
        assert(out.selector == 0 && out.node == node && mt::admin::mutation_protocol::validMutation(out));
        assert(wire.bytes == original);  // Parsing never rewrites the authenticated frame/passkey.
    }
}

void testReservedNodeNumbersRefuseWithoutChangingOutput() {
    for (uint32_t node : {0u, 1u, 0xfffffffeu, 0xffffffffu}) {
        assertRejectedUnchanged(favoriteRequest(node, true));
        assertRejectedUnchanged(favoriteRequest(node, false));
    }
}

void testOtherAdminOperationsRemainExcluded() {
    auto admin = baseAdmin();
    admin.which_payload_variant = meshtastic_AdminMessage_remove_by_nodenum_tag;
    admin.remove_by_nodenum = 0x12345678u;
    assertRejectedUnchanged(encodeRequest(admin));

    admin = baseAdmin();
    admin.which_payload_variant = meshtastic_AdminMessage_set_ignored_node_tag;
    admin.set_ignored_node = 0x12345678u;
    assertRejectedUnchanged(encodeRequest(admin));
}

void testMalformedFavoriteEncodingAndDuplicatesRefuse() {
    Wire base = encodeAdmin([] {
        auto admin = baseAdmin();
        admin.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
        admin.set_favorite_node = 0x12345678u;
        return admin;
    }());

    // The Admin oneof is field 39, wire type 0. A duplicate must fail canonical
    // round-trip equality even when both values happen to agree.
    Wire duplicate = base;
    appendVarint(duplicate, (39u << 3) | 0u);
    appendVarint(duplicate, 0x12345678u);
    assertRejectedUnchanged(encodeData(duplicate.bytes.data(), duplicate.size));

    // Wrong field wire type (length-delimited rather than uint32 varint).
    Wire wrongWire = base;
    appendVarint(wrongWire, (39u << 3) | 2u);
    appendVarint(wrongWire, 1u);
    wrongWire.bytes[wrongWire.size++] = 0x01;
    assertRejectedUnchanged(encodeData(wrongWire.bytes.data(), wrongWire.size));

    // Truncated uint32 varint.
    Wire truncated = base;
    appendVarint(truncated, (39u << 3) | 0u);
    truncated.bytes[truncated.size++] = 0x80;
    assertRejectedUnchanged(encodeData(truncated.bytes.data(), truncated.size));
}

void testPasskeyWidthAndRequestEnvelopeStayUnchanged() {
    Mutation out{};
    auto admin = baseAdmin();
    admin.which_payload_variant = meshtastic_AdminMessage_set_favorite_node_tag;
    admin.set_favorite_node = 0x12345678u;
    admin.session_passkey.size = 7;
    assertRejectedUnchanged(encodeRequest(admin));

    // The classifier checks width only. The caller compares these eight bytes
    // against the live session passkey before any mutation.
    admin.session_passkey.size = 8;
    auto valid = encodeRequest(admin);
    assert(canonicalSetRequest(valid.bytes.data(), valid.size, out));
    assert(out.operation == Operation::Favorite && out.node == 0x12345678u);

    assertRejectedUnchanged(encodeRequest(admin, false, 0, meshtastic_PortNum_ADMIN_APP,
                                          false, 0));
    assertRejectedUnchanged(encodeRequest(admin, true, 99));
    assertRejectedUnchanged(encodeRequest(admin, true, 0, meshtastic_PortNum_TEXT_MESSAGE_APP));
}

void testExistingSetOperationsRetainTheirDecodedShape() {
    auto admin = baseAdmin();
    admin.which_payload_variant = meshtastic_AdminMessage_set_owner_tag;
    Wire wire = encodeRequest(admin);
    Mutation out{};
    assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::Owner && out.selector == 0 && out.node == 0);

    admin = baseAdmin();
    admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
    admin.set_config.which_payload_variant = meshtastic_Config_device_tag;
    wire = encodeRequest(admin);
    assert(canonicalSetRequest(wire.bytes.data(), wire.size, out));
    assert(out.operation == Operation::Config && out.selector == meshtastic_Config_device_tag && out.node == 0);
}
}  // namespace

int main() {
    testBothFavoriteActionsKeepFullNodeNumber();
    testReservedNodeNumbersRefuseWithoutChangingOutput();
    testOtherAdminOperationsRemainExcluded();
    testMalformedFavoriteEncodingAndDuplicatesRefuse();
    testPasskeyWidthAndRequestEnvelopeStayUnchanged();
    testExistingSetOperationsRetainTheirDecodedShape();
    return 0;
}
