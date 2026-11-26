// SPDX-License-Identifier: GPL-3.0-only
// Bounded RAM-only authenticated replay window for the stock-selected policy.

#ifndef MT_PKI_PKIREPLAYWINDOW_H
#define MT_PKI_PKIREPLAYWINDOW_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MtSignature.h"

namespace mt {
namespace pki {

class PkiReplayWindow {
 public:
  static constexpr size_t PeerLimit = 4;
  static constexpr size_t IdLimit = 64;

  enum class Result : uint8_t { Unknown, Fresh, Seen };

  PkiReplayWindow() = default;
  PkiReplayWindow(const PkiReplayWindow &) = delete;
  PkiReplayWindow &operator=(const PkiReplayWindow &) = delete;
  PkiReplayWindow(PkiReplayWindow &&) = delete;
  PkiReplayWindow &operator=(PkiReplayWindow &&) = delete;
  ~PkiReplayWindow() { clear(); }

  // Bind an explicit peer slot to one validated identity. A caller must
  // explicitly unbind before replacing it. Repeating the exact bind preserves
  // all replay history. localNode is supplied by the authenticated owner.
  bool bind(size_t index, uint32_t node, const uint8_t publicKey[32],
            uint32_t localNode) {
    return bindImpl(index,node,publicKey,localNode,true);
  }
  // Only for a node/key pair already admitted by durable owner authority.
  // Unknown RF senders continue through strict bind() and signature admission.
  bool bindTrusted(size_t index,uint32_t node,const uint8_t publicKey[32],uint32_t localNode) {
    return bindImpl(index,node,publicKey,localNode,false);
  }
private:
  bool bindImpl(size_t index,uint32_t node,const uint8_t publicKey[32],uint32_t localNode,bool requireNodeBinding) {
    if (index >= PeerLimit || !publicKey || !validNode(node) ||
        !validNode(localNode) || node == localNode ||
        (requireNodeBinding && bench::MtSignature::nodeId(publicKey) != node)) {
      return false;
    }
    uint8_t converted[32]{};
    struct Wipe {
      uint8_t *bytes;
      ~Wipe() { erase(bytes, 32); }
      static void erase(uint8_t *p, size_t n) {
        volatile uint8_t *v = p;
        while (n--) *v++ = 0;
      }
    } wipe{converted};
    if (!bench::MtSignature::admissiblePublicKey(publicKey, converted)) {
      return false;
    }

    Peer &peer = peers_[index];
    if (peer.bound) {
      return peer.node == node && memcmp(peer.publicKey, publicKey, 32) == 0;
    }
    for (size_t i = 0; i < PeerLimit; ++i) {
      if (i != index && peers_[i].bound &&
          (peers_[i].node == node ||
           memcmp(peers_[i].publicKey, publicKey, 32) == 0)) {
        return false;
      }
    }
    peer.node = node;
    memcpy(peer.publicKey, publicKey, 32);
    peer.bound = 1;
    return true;
  }

  // Unbind is the only operation that permits a slot to acquire a new key;
  // replay IDs and the old public key are wiped together.
public:
  bool unbind(size_t index) {
    if (index >= PeerLimit || !peers_[index].bound) return false;
    erase(&peers_[index], sizeof(Peer));
    return true;
  }

  // Wipe the complete object representation, including padding. Rebooting or
  // clearing this RAM-only policy starts a new replay domain by explicit
  // stock policy; it is not persistent history or a non-resetting guarantee.
  void clear() { erase(this, sizeof(*this)); }

  // Unknown includes invalid IDs, invalid indices, unbound slots, and any
  // peer/key mismatch. The caller must authenticate before commitAuthenticated.
  Result contains(size_t index, uint32_t node, const uint8_t publicKey[32],
                  uint32_t packetId) const {
    const Peer *peer = matching(index, node, publicKey, packetId);
    if (!peer) return Result::Unknown;
    for (size_t i = 0; i < peer->count; ++i) {
      const size_t at = (size_t(peer->oldest) + i) % IdLimit;
      if (peer->ids[at] == packetId) return Result::Seen;
    }
    return Result::Fresh;
  }

  // Call only after packet authentication. Duplicate observation never moves
  // the FIFO age; a new ID overwrites only the oldest ID for that peer.
  Result commitAuthenticated(size_t index, uint32_t node,
                             const uint8_t publicKey[32], uint32_t packetId) {
    const Result prior = contains(index, node, publicKey, packetId);
    if (prior != Result::Fresh) return prior;
    Peer &peer = peers_[index];
    if (peer.count < IdLimit) {
      const size_t at = (size_t(peer.oldest) + peer.count) % IdLimit;
      peer.ids[at] = packetId;
      ++peer.count;
    } else {
      peer.ids[peer.oldest] = packetId;
      peer.oldest = uint8_t((size_t(peer.oldest) + 1) % IdLimit);
    }
    return Result::Fresh;
  }

 private:
  struct Peer {
    uint32_t node = 0;
    uint8_t publicKey[32]{};
    uint32_t ids[IdLimit]{};
    uint8_t oldest = 0;
    uint8_t count = 0;
    uint8_t bound = 0;
  };

  static bool validNode(uint32_t node) { return node >= 2 && node <= 0xfffffffdu; }
  static void erase(void *p, size_t n) {
    volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
    while (n--) *v++ = 0;
  }

  const Peer *matching(size_t index, uint32_t node, const uint8_t key[32],
                       uint32_t packetId) const {
    if (index >= PeerLimit || !packetId || !key || !validNode(node)) return nullptr;
    const Peer &peer = peers_[index];
    if (!peer.bound || peer.node != node ||
        memcmp(peer.publicKey, key, 32) != 0) return nullptr;
    return &peer;
  }

  Peer peers_[PeerLimit]{};
};

static_assert(sizeof(PkiReplayWindow) <= 1280,
              "Four independent 64-ID replay FIFOs must remain bounded");

}  // namespace pki
}  // namespace mt

#endif  // MT_PKI_PKIREPLAYWINDOW_H
