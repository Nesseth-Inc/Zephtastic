// SPDX-License-Identifier: GPL-3.0-only
// Qwen proposal integrated with pinned include paths and canonical empty input.
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

namespace mt {

struct NeighborInfoPayload {
  static bool decode(const uint8_t *data, size_t size,
                     meshtastic_NeighborInfo &out) {
    if (data == nullptr && size) return false;
    const uint8_t empty=0;if(!data)data=&empty;
    if (size > static_cast<size_t>(meshtastic_NeighborInfo_size)) return false;

    meshtastic_NeighborInfo tmp{};
    pb_istream_t in = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t *>(data), size);
    if (!pb_decode(&in, meshtastic_NeighborInfo_fields, &tmp)) return false;
    if (in.bytes_left != 0) return false;

    const pb_size_t kMaxNeighbors =
        sizeof(tmp.neighbors) / sizeof(tmp.neighbors[0]);
    if (tmp.neighbors_count > kMaxNeighbors) return false;
    for (pb_size_t i = 0; i < tmp.neighbors_count; ++i) {
      if (!isfinite(tmp.neighbors[i].snr)) return false;
    }

    uint8_t buf[meshtastic_NeighborInfo_size];
    pb_ostream_t enc = pb_ostream_from_buffer(
        reinterpret_cast<pb_byte_t *>(buf), sizeof(buf));
    if (!pb_encode(&enc, meshtastic_NeighborInfo_fields, &tmp)) return false;
    if (enc.bytes_written != size) return false;
    if (size > 0 && memcmp(buf, data, size) != 0) return false;

    out = tmp;
    return true;
  }
};

}  // namespace mt
