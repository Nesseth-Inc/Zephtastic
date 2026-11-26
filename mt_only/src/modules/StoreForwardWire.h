// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "meshtastic/storeforward.pb.h"

namespace mt::store_forward {

// Bounded codec for the pinned stock port-65 StoreAndForward protobuf.
// This layer owns no packet, routing, radio, or requester policy.
class StoreForwardWire {
public:
  static constexpr size_t MaxTextBytes = 233;
  static constexpr size_t MaxEncodedBytes = meshtastic_StoreAndForward_size;

  enum class Error : uint8_t {
    None,
    InvalidArgument,
    Malformed,
    TooLarge,
    NoSpace,
    InvalidMessage,
  };

  // Decode transactionally: on failure, *out remains unchanged. Unknown rr
  // numeric values are preserved (stock dispatch ignores unknown values).
  // Nanopb's protobuf oneof rule applies: if malformed input repeats oneof
  // fields, the last decoded alternative wins. rr and variant are independent
  // fields, matching stock handling; semantic pair validation belongs to the
  // control layer, which checks the variant only for rr values that use it.
  static bool decode(const uint8_t *input, size_t inputSize,
                     meshtastic_StoreAndForward *out, Error *error = nullptr);

  // Encodes into a 244-byte bounded temporary first. Failure leaves output
  // bytes untouched and sets *written to zero. `window` is passed through as
  // the raw uint32 field; stock interprets request and response units
  // differently, so this codec intentionally performs no conversion.
  static bool encode(const meshtastic_StoreAndForward &message,
                     uint8_t *output, size_t outputCapacity, size_t *written,
                     Error *error = nullptr);
};

static_assert(StoreForwardWire::MaxTextBytes == 233,
              "pinned stock StoreForward text bound");
static_assert(StoreForwardWire::MaxEncodedBytes == 244,
              "pinned stock StoreForward max wire size");

}  // namespace mt::store_forward
