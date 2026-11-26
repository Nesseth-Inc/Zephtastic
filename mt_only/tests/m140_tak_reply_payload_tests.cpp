// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"
#include "../src/modules/TakPliReplyPayload.h"

static mt::tak_pli::Inputs inputFor(meshtastic_Position &position)
{
    mt::tak_pli::Inputs in{};
    in.position = &position;
    in.ownerLongName = "Reply Unit";
    in.nodeNumber = 0x12345678;
    in.batteryPercent = 82;
    in.configuredTeam = meshtastic_Team_Green;
    in.configuredRole = meshtastic_MemberRole_Medic;
    in.hasTakConfig = true;
    return in;
}

static meshtastic_Data decodeData(const uint8_t *bytes, size_t size)
{
    meshtastic_Data data = meshtastic_Data_init_zero;
    pb_istream_t in = pb_istream_from_buffer(bytes, size);
    assert(pb_decode(&in, meshtastic_Data_fields, &data));
    assert(in.bytes_left == 0);
    return data;
}

int main()
{
    meshtastic_Position position = meshtastic_Position_init_zero;
    position.latitude_i = 475123456;
    position.longitude_i = -1221234567;
    position.altitude_hae = 321;
    position.ground_speed = 23;
    position.ground_track = 2712000;
    position.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;

    const auto input = inputFor(position);
    uint8_t wire[mt::tak_pli_reply::PlaintextLimit + 2];
    memset(wire, 0xa7, sizeof(wire));
    size_t written = 77;
    constexpr uint32_t requestId = 0x89abcdef;
    assert(mt::tak_pli_reply::encode(input, requestId, wire,
                                     mt::tak_pli_reply::PlaintextLimit, written) ==
           mt::tak_pli_reply::Result::Ok);
    assert(written > 0 && written <= mt::tak_pli_reply::PlaintextLimit);
    const size_t validWritten = written;
    assert(wire[mt::tak_pli_reply::PlaintextLimit] == 0xa7 &&
           wire[mt::tak_pli_reply::PlaintextLimit + 1] == 0xa7);

    meshtastic_Data data = decodeData(wire, written);
    assert(data.portnum == meshtastic_PortNum_ATAK_PLUGIN_V2);
    assert(data.request_id == requestId);
    assert(data.reply_id == 0);
    assert(!data.want_response);
    assert(data.dest == 0 && data.source == 0 && data.emoji == 0);
    assert(data.has_bitfield && data.bitfield == 0);
    assert(data.xeddsa_signature.size == 0);
    assert(data.payload.size > 1 && data.payload.bytes[0] == 0xff);

    meshtastic_TAKPacketV2 pli = meshtastic_TAKPacketV2_init_zero;
    pb_istream_t pliStream = pb_istream_from_buffer(data.payload.bytes + 1,
                                                    data.payload.size - 1);
    assert(pb_decode(&pliStream, &meshtastic_TAKPacketV2_msg, &pli));
    assert(pliStream.bytes_left == 0);
    assert(pli.cot_type_id == meshtastic_CotType_CotType_a_f_G_U_C);
    assert(pli.team == meshtastic_Team_Green);
    assert(pli.role == meshtastic_MemberRole_Medic);
    assert(pli.latitude_i == position.latitude_i);
    assert(pli.longitude_i == position.longitude_i);
    assert(pli.altitude == position.altitude_hae);
    assert(pli.speed == position.ground_speed);
    assert(pli.course == 2712);
    assert(pli.battery == 82);
    assert(strcmp(pli.uid, "!12345678") == 0);

    // Correlation is Data.request_id; reply_id remains zero. Re-encoding the
    // decoded Data must reproduce the canonical wire bytes exactly.
    uint8_t canonical[mt::tak_pli_reply::PlaintextLimit];
    pb_ostream_t canonicalStream = pb_ostream_from_buffer(canonical, sizeof(canonical));
    assert(pb_encode(&canonicalStream, meshtastic_Data_fields, &data));
    assert(canonicalStream.bytes_written == written);
    assert(memcmp(canonical, wire, written) == 0);

    // Insufficient capacity, zero request ID, null output, and invalid PLI
    // inputs all fail transactionally: written is zero and output is intact.
    uint8_t canary[mt::tak_pli_reply::PlaintextLimit];
    memset(canary, 0x5c, sizeof(canary));
    written = 55;
    assert(mt::tak_pli_reply::encode(input, requestId, canary, validWritten - 1, written) ==
           mt::tak_pli_reply::Result::OutputTooSmall);
    assert(written == 0);
    for (uint8_t b : canary) assert(b == 0x5c);

    memset(canary, 0x3d, sizeof(canary));
    written = 99;
    assert(mt::tak_pli_reply::encode(input, requestId, canary, 1, written) ==
           mt::tak_pli_reply::Result::OutputTooSmall);
    assert(written == 0);
    for (uint8_t b : canary) assert(b == 0x3d);

    memset(canary, 0x6e, sizeof(canary));
    written = 44;
    assert(mt::tak_pli_reply::encode(input, 0, canary, sizeof(canary), written) ==
           mt::tak_pli_reply::Result::InvalidInput);
    assert(written == 0);
    for (uint8_t b : canary) assert(b == 0x6e);

    written = 44;
    assert(mt::tak_pli_reply::encode(input, requestId, nullptr, 0, written) ==
           mt::tak_pli_reply::Result::InvalidInput);
    assert(written == 0);

    auto invalid = input;
    invalid.position = nullptr;
    memset(canary, 0x91, sizeof(canary));
    written = 44;
    assert(mt::tak_pli_reply::encode(invalid, requestId, canary, sizeof(canary), written) ==
           mt::tak_pli_reply::Result::PliEncodeFailed);
    assert(written == 0);
    for (uint8_t b : canary) assert(b == 0x91);

    // The encoder returns Data plaintext only. It does not create a MeshPacket
    // ACK bit, fresh outer packet ID, channel envelope, or radio frame.
    return 0;
}
