// SPDX-License-Identifier: GPL-3.0-only
#include "StoreForwardWire.h"

#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"

// The pinned ZephCore Protobuf.cpp does not bind this upstream stock schema.
// Keep its descriptor here so neither vendored generated headers nor the
// general descriptor registry need modification.
PB_BIND(meshtastic_StoreAndForward, meshtastic_StoreAndForward, 4)
PB_BIND(meshtastic_StoreAndForward_Statistics,
        meshtastic_StoreAndForward_Statistics, 4)
PB_BIND(meshtastic_StoreAndForward_History, meshtastic_StoreAndForward_History, 4)
PB_BIND(meshtastic_StoreAndForward_Heartbeat,
        meshtastic_StoreAndForward_Heartbeat, 4)

namespace mt::store_forward {
namespace {
using RequestResponse = meshtastic_StoreAndForward_RequestResponse;

static_assert(meshtastic_StoreAndForward_RequestResponse_UNSET == 0 &&
                  meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT == 106,
              "pinned StoreForward rr enum range changed");
static_assert(sizeof(RequestResponse) == sizeof(uint8_t) ||
                  sizeof(RequestResponse) == sizeof(uint32_t),
              "unsupported StoreForward rr enum storage width");

// Do not evaluate an untrusted enum as a C++ enum: UBSan diagnoses values
// outside the enum's representable range before a caller's default switch
// can ignore them. The generated enum's current enumerator range 0..106 has
// the C++ range 0..127. Unknown values within that range remain stock-style
// default/no-op values; values outside it are rejected at this boundary.
uint32_t rawRequestResponse(const RequestResponse &value) {
  if (sizeof(value) == sizeof(uint8_t)) {
    uint8_t raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    return raw;
  }
  uint32_t raw = 0;
  memcpy(&raw, &value, sizeof(raw));
  return raw;
}

bool requestResponseInRange(const RequestResponse &value) {
  return rawRequestResponse(value) <= 127;
}

void setError(StoreForwardWire::Error *out, StoreForwardWire::Error value) {
  if (out) *out = value;
}
}  // namespace

bool StoreForwardWire::decode(const uint8_t *input, size_t inputSize,
                              meshtastic_StoreAndForward *out, Error *error) {
  setError(error, Error::InvalidArgument);
  if ((!input && inputSize != 0) || !out || inputSize > MaxEncodedBytes) {
    if (inputSize > MaxEncodedBytes) setError(error, Error::TooLarge);
    return false;
  }

  meshtastic_StoreAndForward decoded = meshtastic_StoreAndForward_init_zero;
  // Some nanopb buffer callbacks expect a non-null pointer even for size zero.
  // A valid empty protobuf message has no bytes to read, so provide stable
  // storage while preserving the pinned decoder's default-message semantics.
  static const uint8_t emptyInput = 0;
  pb_istream_t stream =
      pb_istream_from_buffer(input ? input : &emptyInput, inputSize);
  if (!pb_decode(&stream, meshtastic_StoreAndForward_fields, &decoded) ||
      stream.bytes_left != 0) {
    setError(error, Error::Malformed);
    return false;
  }
  if (!requestResponseInRange(decoded.rr)) {
    setError(error, Error::Malformed);
    return false;
  }
  if (decoded.which_variant == meshtastic_StoreAndForward_text_tag &&
      decoded.variant.text.size > MaxTextBytes) {
    setError(error, Error::TooLarge);
    return false;
  }
  *out = decoded;
  setError(error, Error::None);
  return true;
}

bool StoreForwardWire::encode(const meshtastic_StoreAndForward &message,
                              uint8_t *output, size_t outputCapacity,
                              size_t *written, Error *error) {
  setError(error, Error::InvalidArgument);
  if (written) *written = 0;
  if (!output || !written) return false;
  if (!requestResponseInRange(message.rr)) {
    setError(error, Error::InvalidMessage);
    return false;
  }
  if (message.which_variant == meshtastic_StoreAndForward_text_tag &&
      message.variant.text.size > MaxTextBytes) {
    setError(error, Error::TooLarge);
    return false;
  }
  if (message.which_variant != 0 &&
      message.which_variant != meshtastic_StoreAndForward_stats_tag &&
      message.which_variant != meshtastic_StoreAndForward_history_tag &&
      message.which_variant != meshtastic_StoreAndForward_heartbeat_tag &&
      message.which_variant != meshtastic_StoreAndForward_text_tag) {
    setError(error, Error::InvalidMessage);
    return false;
  }

  uint8_t encoded[MaxEncodedBytes];
  pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
  if (!pb_encode(&stream, meshtastic_StoreAndForward_fields, &message)) {
    setError(error, Error::InvalidMessage);
    return false;
  }
  if (stream.bytes_written > outputCapacity) {
    setError(error, Error::NoSpace);
    return false;
  }
  memcpy(output, encoded, stream.bytes_written);
  *written = stream.bytes_written;
  setError(error, Error::None);
  return true;
}
}  // namespace mt::store_forward
