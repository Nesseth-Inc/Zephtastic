// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "meshtastic/mesh.pb.h"
#include "meshtastic/paxcount.pb.h"
#include <pb_encode.h>
#include <string.h>

namespace mt::modules::pax {
static_assert(meshtastic_PortNum_PAXCOUNTER_APP == 34);
inline constexpr uint8_t Request[] = {8, 34, 24, 1, 72, 2};
inline bool isRequest(const uint8_t *data, size_t size) {
  // Router::perhapsEncode adds the originating node's MQTT consent bit.
  return data && size == sizeof(Request) && (data[size - 1] & ~1u) == 2 &&
         !memcmp(data, Request, size - 1);
}

inline bool encodeReply(uint32_t request, const meshtastic_Paxcount &count, uint8_t *out, size_t capacity, size_t &size) {
  size = 0;
  if (!request || !out) return false;
  meshtastic_Data data{};
  data.portnum = meshtastic_PortNum_PAXCOUNTER_APP;
  data.request_id = request;
  data.has_bitfield = true;
  auto payload = pb_ostream_from_buffer(data.payload.bytes, sizeof(data.payload.bytes));
  if (!pb_encode(&payload, meshtastic_Paxcount_fields, &count) || !payload.bytes_written) return false;
  data.payload.size = payload.bytes_written;
  auto wire = pb_ostream_from_buffer(out, capacity);
  if (!pb_encode(&wire, meshtastic_Data_fields, &data)) return false;
  size = wire.bytes_written;
  return true;
}
}
