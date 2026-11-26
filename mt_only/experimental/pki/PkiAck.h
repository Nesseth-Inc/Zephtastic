// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiCrypto.h"

namespace mt::experimental {
// Protocol primitive: no key lookup/storage, routing or delivery policy.
// Runtime orchestration is explicitly injected; default startup has no provider.
struct AckEnvelope {
  bool decodedRouting = false;
  uint32_t from = 0, to = 0, request = 0;
};
struct PendingAck {
  bool active = false;
  uint32_t peer = 0, local = 0, request = 0;
};
enum class ProofFieldResult { Present, Absent, Malformed };
struct ProofField {
  uint8_t proof[8]{};
  size_t start = 0, length = 0;
};
enum class AckProofVerdict {
  ValidProof, Absent, Malformed, NoTrustedKey, ContextMismatch, InvalidProof,
  InvalidInput
};
class PkiAck {
  PkiCrypto &crypto_;
public:
  static constexpr size_t MaxRouting = 233, FieldSize = 10;
  explicit PkiAck(PkiCrypto &crypto):crypto_(crypto){}
  // Does not decode/re-encode Routing. Unknown fields remain MAC-covered verbatim.
  // Repeated proof fields, wrong proof wire type/length, and malformed input fail.
  // Output field is unchanged unless the whole bounded stream is valid/present.
  static ProofFieldResult inspect(const uint8_t *wire,size_t n,ProofField &field);
  bool attach(const uint8_t secret[32],const uint8_t trustedPeer[32],
              const AckEnvelope &context,const uint8_t *routing,size_t n,
              uint8_t *out,size_t cap,size_t &len);
  // Caller must supply the authoritative key of pending.peer, not look up a key
  // using the untrusted envelope.from. This API cannot establish that binding.
  // Cheap pending/context checks occur before any X25519. A ValidProof authenticates
  // these bytes to that pairwise key; it is NOT a success ACK or delivery verdict.
  // Caller still decodes Routing error/variant and owns expiry/replay/retry policy.
  // No output bytes are published on failure; len is always set to zero then.
  AckProofVerdict verify(const uint8_t secret[32],const uint8_t trustedPeer[32],
                        const PendingAck &pending,const AckEnvelope &received,
                        const uint8_t *wire,size_t n,uint8_t *routing,
                        size_t cap,size_t &len);
};
}
