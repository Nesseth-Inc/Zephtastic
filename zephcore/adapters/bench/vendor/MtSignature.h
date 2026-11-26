// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtCodec.h"

namespace bench {
// Public signature checking for the pinned 2.8.1 XEdDSA envelope. This is a
// COMPATIBLE relay subset: unsigned channel data stays unauthenticated; signed
// non-NodeInfo from an unknown key is restricted to opaque forwarding. Neither
// endpoint PKI decryption nor pairwise ACK proof verification is implemented.
// Main-thread ownership only; key material here is public, never private.
class MtSignature {
public:
  static constexpr unsigned Capacity=32;
  static constexpr uint32_t KeyTtlMs=30u*60u*1000u;
  static constexpr size_t SigningHeaderLength=34;
  enum class Result : uint8_t { Unsigned, Verified, Quarantined, Rejected };
private:
  struct Entry {
    uint32_t node=0, seen=0;
    uint8_t curve[32]{}, ed[32]{};
  } entries_[Capacity]{};
  static uint32_t read32(const uint8_t *p);
  static void write32(uint8_t *p,uint32_t v);
  static bool live(const Entry &e,uint32_t now) {
    return e.node && uint32_t(now-e.seen)<KeyTtlMs;
  }
public:
  void clear();
  // Explicit verified local NodeDB removal only. Forget one public authority;
  // subsequent learning still uses apply()'s existing signature/ID checks.
  void forgetPublicKey(uint32_t node);
  unsigned keyCount(uint32_t now) const;
  // Read an existing live public authority without learning or refreshing it.
  bool publicIdentity(uint32_t node,uint32_t now,uint8_t out[32])const;
  // Explicit RAM-only operator pinning permits legacy MAC-derived node IDs.
  // Caller must enforce disarmed configuration; no opportunistic key trust.
  bool trustPublicKey(uint32_t node,const uint8_t key[32],uint32_t now);
  static uint32_t nodeId(const uint8_t publicKey[32]);
  static size_t signingBuffer(uint32_t from,uint32_t id,uint32_t to,
                              const RouteMetadata &m,uint8_t *out,size_t cap);
  // Stricter admission than upstream's field-element conversion: canonical
  // X25519 encoding and a nonzero X25519 result with a fixed PUBLIC validation
  // scalar. This detects low-order keys without any endpoint secret/session.
  static bool admissiblePublicKey(const uint8_t curve[32],uint8_t ed[32]);
  // Only the channel owner may opt in after resolving this exact frame to a
  // configured shared channel; this verifier never decrypts a PKI candidate.
  Result apply(const uint8_t *frame,size_t n,RouteMetadata &m,uint32_t now,
               bool allowResolvedZeroUnicast=false);
};
} // namespace bench
