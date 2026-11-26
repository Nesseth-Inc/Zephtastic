// SPDX-License-Identifier: GPL-3.0-only
//
// PkiPeerSet: fixed-capacity (16), RAM-only operator pin set. Zero
// allocation, no I/O, no trust learning, no persistence, no private
// keys. Supplied public values are validated. API context
// (INPUTS.txt): mt::pki::IdentityBinding (PkiIdentity.h),
// bench::MtSignature (MtSignature.h).

#ifndef MT_PKI_PKIPEERSET_H
#define MT_PKI_PKIPEERSET_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MtSignature.h"
#include "PkiIdentity.h"
#include "PkiCapacity.h"

namespace mt {
namespace pki {

struct PkiPeerPin {
  uint32_t node = 0;
  uint8_t publicKey[32]{};
};

class PkiPeerSet {
 public:
  static constexpr size_t Limit = ContactLimit;

  // Explicit zero-active-peer snapshot, with the same local-domain validation
  // as assign(). It grants no trust or runtime authority on its own.
  bool assignEmpty(const IdentityBinding &local) {
    uint8_t scratch[32] = {};
    if (!validLocal(local, scratch)) {
      return false;
    }
    clear();
    return true;
  }

  // Atomically replace the pin set. Every input is validated before any
  // mutation; on failure the previous set is unchanged. Caller pointers
  // are never retained: accepted pins are copied into fixed storage.
  bool assign(const IdentityBinding &local, const PkiPeerPin *pins,
              size_t count) {
    return assignValidated(local, pins, count, true);
  }

  // Assign an explicitly trusted owner/ledger node-key mapping. Unlike
  // assign(), the peer node is not required to equal CRC32(publicKey); all
  // local-identity, key-admissibility, reserved/self-ID, and uniqueness
  // checks remain mandatory. Callers must already have durable authority
  // for these mappings; this method itself does not confer that authority.
  bool assignTrusted(const IdentityBinding &local, const PkiPeerPin *pins,
                     size_t count) {
    return assignValidated(local, pins, count, false);
  }

  // Erase all pins and reset the count. No I/O, no allocation.
  void clear() {
    for (size_t i = 0; i < Limit; ++i) {
      pins_[i] = PkiPeerPin{};
    }
    count_ = 0;
  }

  size_t count() const {
    return count_;
  }

  // Null when index is out of bounds.
  const PkiPeerPin *at(size_t index) const {
    return index < count_ ? &pins_[index] : nullptr;
  }

  // Null when the node is absent.
  const PkiPeerPin *find(uint32_t node) const {
    for (size_t i = 0; i < count_; ++i) {
      if (pins_[i].node == node) {
        return &pins_[i];
      }
    }
    return nullptr;
  }

 private:
  static bool validLocal(const IdentityBinding &local, uint8_t scratch[32]) {
    return local.version == 1 && local.algorithm == 1 &&
           local.generation != 0 && validNode(local.node) &&
           bench::MtSignature::admissiblePublicKey(local.publicKey, scratch) &&
           bench::MtSignature::nodeId(local.publicKey) == local.node;
  }

#if defined(__GNUC__) && !defined(__clang__)
  __attribute__((noinline,noclone))
#else
  [[gnu::noinline]]
#endif
  bool assignValidated(const IdentityBinding &local, const PkiPeerPin *pins,
                       size_t count, bool requirePeerCrcNode) {
    if (pins == nullptr || count < 1 || count > Limit) {
      return false;
    }
    uint8_t scratch[32] = {};
    if (!validLocal(local, scratch)) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const PkiPeerPin &p = pins[i];
      if (!validNode(p.node) || p.node == local.node ||
          sameKey(p.publicKey, local.publicKey)) {
        return false;
      }
      if (!bench::MtSignature::admissiblePublicKey(p.publicKey, scratch) ||
          (requirePeerCrcNode &&
           bench::MtSignature::nodeId(p.publicKey) != p.node)) {
        return false;
      }
      for (size_t j = 0; j < i; ++j) {
        if (pins[j].node == p.node ||
            sameKey(p.publicKey, pins[j].publicKey)) {
          return false;
        }
      }
    }
    for (size_t i = 0; i < Limit; ++i) {
      pins_[i] = i < count ? pins[i] : PkiPeerPin{};
    }
    count_ = count;
    return true;
  }

  // Node IDs 0 and 1 are reserved; 0xfffffffe/0xffffffff are excluded.
  static bool validNode(uint32_t node) {
    return node >= 2 && node <= 0xfffffffd;
  }

  static bool sameKey(const uint8_t a[32], const uint8_t b[32]) {
    return memcmp(a, b, 32) == 0;
  }

  PkiPeerPin pins_[Limit]{};
  size_t count_ = 0;
};

}  // namespace pki
}  // namespace mt

#endif  // MT_PKI_PKIPEERSET_H
