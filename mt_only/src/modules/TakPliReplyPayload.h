// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "TakPliPayload.h"
#include "meshtastic/mesh.pb.h"
#include "pb_encode.h"

namespace mt {
namespace tak_pli_reply {

// Serialized meshtastic_Data plaintext budget. This is the bounded caller
// contract, including Data fields and the inner raw-PLI payload.
static constexpr size_t PlaintextLimit = 239;
static constexpr size_t DataEncodeScratchLimit = 256;
static constexpr uint8_t RawProtobufMarker = tak_pli::RawProtobufMarker;

static_assert(tak_pli::PayloadLimit == sizeof(((meshtastic_Data *)0)->payload.bytes),
              "TAK payload capacity must match generated Data payload capacity");

enum class Result : uint8_t {
    Ok,
    InvalidInput,
    PliEncodeFailed,
    DataEncodeFailed,
    PlaintextTooLarge,
    OutputTooSmall
};

// Produces only the serialized Data layer for a stock-compatible role-10
// position reply. MeshPacket ID allocation, destination/channel/ACK/hop,
// encryption and transmission remain caller responsibilities.
inline Result encode(const tak_pli::Inputs &inputs, uint32_t requestPacketId,
                     uint8_t *output, size_t outputCapacity, size_t &written)
{
    written = 0;
    if (!output || requestPacketId == 0)
        return Result::InvalidInput;

    uint8_t pliPayload[tak_pli::PayloadLimit];
    size_t pliSize = 0;
    const tak_pli::Result pliResult =
        tak_pli::encode(inputs, pliPayload, sizeof(pliPayload), pliSize);
    if (pliResult != tak_pli::Result::Ok || pliSize == 0 ||
        pliSize > sizeof(pliPayload))
        return Result::PliEncodeFailed;

    meshtastic_Data data = meshtastic_Data_init_zero;
    data.portnum = meshtastic_PortNum_ATAK_PLUGIN_V2;
    data.payload.size = static_cast<pb_size_t>(pliSize);
    memcpy(data.payload.bytes, pliPayload, pliSize);
    data.want_response = false;
    data.dest = 0;
    data.source = 0;
    data.request_id = requestPacketId;
    data.reply_id = 0;
    data.emoji = 0;
    // Match the project's canonical Data representation. This is a Data
    // payload property; MeshPacket.want_ack is selected by the caller.
    data.has_bitfield = true;
    data.bitfield = 0;

    uint8_t staged[DataEncodeScratchLimit];
    pb_ostream_t stream = pb_ostream_from_buffer(staged, sizeof(staged));
    if (!pb_encode(&stream, meshtastic_Data_fields, &data) ||
        stream.bytes_written == 0)
        return Result::DataEncodeFailed;
    if (stream.bytes_written > PlaintextLimit)
        return Result::PlaintextTooLarge;
    if (stream.bytes_written > outputCapacity)
        return Result::OutputTooSmall;

    memcpy(output, staged, stream.bytes_written);
    written = stream.bytes_written;
    return Result::Ok;
}

} // namespace tak_pli_reply
} // namespace mt
