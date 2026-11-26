// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Bounded correlation state for locally initiated private Admin GETs.
// Service owns authentication/replay provenance and calls responseOwners only
// with an authenticated key and response packet ID. consume=true is permitted
// only after durable Fresh replay commit, while serialized under its lock.

#include "admin/RemoteAdminProtocol.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::admin {

class AdminQueryWindow {
 public:
  static constexpr size_t SlotCount = 4;
  static constexpr size_t PublicKeyBytes = 32;
  static constexpr uint32_t FirstUnicastNode = 2;
  static constexpr uint32_t ReservedNodeBoundary = 0xfffffffeu;
  static constexpr uint32_t MaximumLifetimeMs = 0x7fffffffu;

  enum class RecordResult : uint8_t {
    Recorded,
    MergedOwner,
    Invalid,
    IdCollision,
    Full,
  };

  // Must be called only after the local private request has actually entered
  // the transport queue. requestId is its local PKI packet ID, copied by the
  // remote stock Admin reply into Data.request_id. A duplicate ID may merge
  // owners only for the exact same peer, current public key, and query.
  RecordResult recordConfirmedEnqueue(uint32_t peerNode,
                                     const uint8_t *peerPublicKey,
                                     uint32_t requestId,
                                     const remote_protocol::Query &query,
                                     uint32_t ownerMask,
                                     uint32_t now,
                                     uint32_t lifetimeMs) {
    expire(now);
    size_t freeIndex = SlotCount;
    RecordResult result = assess(peerNode, peerPublicKey, requestId, query,
                                 ownerMask, lifetimeMs, &freeIndex);
    if (result == RecordResult::MergedOwner) {
      slots_[freeIndex].ownerMask |= ownerMask;
      return result;
    }
    if (result != RecordResult::Recorded) return result;
    if (freeIndex == SlotCount) return RecordResult::Full;

    Slot &slot = slots_[freeIndex];
    slot.occupied = true;
    slot.peerNode = peerNode;
    slot.requestId = requestId;
    slot.ownerMask = ownerMask;
    slot.startedAt = now;
    slot.lifetimeMs = lifetimeMs;
    slot.query = query;
    memcpy(slot.peerPublicKey, peerPublicKey, PublicKeyBytes);
    return RecordResult::Recorded;
  }

  // Side-effect-free enqueue preflight apart from expiring stale entries.
  RecordResult canRecord(uint32_t peerNode, const uint8_t *peerPublicKey,
                         uint32_t requestId, const remote_protocol::Query &query,
                         uint32_t ownerMask, uint32_t now, uint32_t lifetimeMs) {
    expire(now);
    return assess(peerNode, peerPublicKey, requestId, query, ownerMask,
                  lifetimeMs, nullptr);
  }

  // Probe with consume=false before replay commit; consume=true only after
  // durable Fresh commit in Service's serialized event path.
  uint32_t responseOwners(uint32_t localNode, uint32_t from,
                          const uint8_t *authenticatedKey,
                          uint32_t responsePacketId, const uint8_t *data,
                          size_t size, uint32_t now, bool consume) {
    expire(now);
    if (!isUnicast(localNode) || !isUnicast(from) || from == localNode ||
        !responsePacketId || !data || !size ||
        size > remote_protocol::MaxDataBytes || !validKey(authenticatedKey)) return 0;

    for (size_t i = 0; i < SlotCount; ++i) {
      Slot &slot = slots_[i];
      if (!slot.occupied || from != slot.peerNode ||
          !sameKey(slot.peerPublicKey, authenticatedKey)) {
        continue;
      }
      if (!remote_protocol::canonicalGetResponse(data, size,
                                                  slot.query, slot.requestId)) {
        continue;
      }
      const uint32_t owners = slot.ownerMask;
      if (consume) clearSlot(slot);
      return owners;
    }
    return 0;
  }

  // All intervals are constrained below half the uint32 range; unsigned
  // elapsed-time arithmetic is therefore wrap-safe across millis rollover.
  size_t expire(uint32_t now) {
    size_t removed = 0;
    for (size_t i = 0; i < SlotCount; ++i) {
      Slot &slot = slots_[i];
      if (slot.occupied && uint32_t(now - slot.startedAt) >= slot.lifetimeMs) {
        clearSlot(slot);
        ++removed;
      }
    }
    return removed;
  }

  // Owner masks represent UI/request owners waiting on a merged identical
  // query. Removing the final owner cancels its correlation slot.
  size_t cancelOwners(uint32_t ownerMask) {
    if (!ownerMask) return 0;
    size_t removed = 0;
    for (size_t i = 0; i < SlotCount; ++i) {
      Slot &slot = slots_[i];
      if (!slot.occupied) continue;
      slot.ownerMask &= ~ownerMask;
      if (!slot.ownerMask) {
        clearSlot(slot);
        ++removed;
      }
    }
    return removed;
  }

  // Disconnect/revocation must cancel every outstanding query for that peer.
  size_t cancelPeer(uint32_t peerNode) {
    size_t removed = 0;
    for (size_t i = 0; i < SlotCount; ++i) {
      Slot &slot = slots_[i];
      if (slot.occupied && slot.peerNode == peerNode) {
        clearSlot(slot);
        ++removed;
      }
    }
    return removed;
  }

  // A verified Routing error ends only the query with this exact identity.
  // A merged slot is cleared as one unit so every owner sees the same error.
  bool cancelRequest(uint32_t peerNode, const uint8_t *authenticatedKey,
                     uint32_t requestId) {
    if (!isUnicast(peerNode) || !requestId || !validKey(authenticatedKey))
      return false;
    for (size_t i = 0; i < SlotCount; ++i) {
      Slot &slot = slots_[i];
      if (slot.occupied && slot.peerNode == peerNode &&
          slot.requestId == requestId &&
          sameKey(slot.peerPublicKey, authenticatedKey)) {
        clearSlot(slot);
        return true;
      }
    }
    return false;
  }

  void clear() {
    for (size_t i = 0; i < SlotCount; ++i) clearSlot(slots_[i]);
  }

  size_t size() const {
    size_t used = 0;
    for (size_t i = 0; i < SlotCount; ++i) used += slots_[i].occupied ? 1u : 0u;
    return used;
  }

 private:
  struct Slot {
    uint8_t peerPublicKey[PublicKeyBytes]{};
    remote_protocol::Query query{};
    uint32_t peerNode = 0;
    uint32_t requestId = 0;
    uint32_t ownerMask = 0;
    uint32_t startedAt = 0;
    uint32_t lifetimeMs = 0;
    bool occupied = false;
  };

  Slot slots_[SlotCount]{};

  RecordResult assess(uint32_t peerNode, const uint8_t *peerPublicKey,
                      uint32_t requestId, const remote_protocol::Query &query,
                      uint32_t ownerMask, uint32_t lifetimeMs,
                      size_t *freeIndexOut) const {
    if (!isUnicast(peerNode) || !requestId || !validKey(peerPublicKey) ||
        !remote_protocol::validQuery(query) || !ownerMask || !lifetimeMs ||
        lifetimeMs > MaximumLifetimeMs) return RecordResult::Invalid;
    size_t freeIndex = SlotCount;
    for (size_t i = 0; i < SlotCount; ++i) {
      const Slot &slot = slots_[i];
      if (!slot.occupied) { if (freeIndex == SlotCount) freeIndex = i; continue; }
      if (slot.requestId != requestId) continue;
      if (slot.peerNode != peerNode || !sameKey(slot.peerPublicKey, peerPublicKey) ||
          !sameQuery(slot.query, query)) return RecordResult::IdCollision;
      if (freeIndexOut) *freeIndexOut = i;
      return RecordResult::MergedOwner;
    }
    if (freeIndex == SlotCount) return RecordResult::Full;
    if (freeIndexOut) *freeIndexOut = freeIndex;
    return RecordResult::Recorded;
  }

  static bool isUnicast(uint32_t node) {
    return node >= FirstUnicastNode && node < ReservedNodeBoundary;
  }

  static bool validKey(const uint8_t *key) {
    if (!key) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < PublicKeyBytes; ++i) any |= key[i];
    return any != 0;
  }

  static bool sameKey(const uint8_t *left, const uint8_t *right) {
    return memcmp(left,right,PublicKeyBytes)==0;
  }

  static bool sameQuery(const remote_protocol::Query &left,
                        const remote_protocol::Query &right) {
    return left.operation == right.operation && left.selector == right.selector;
  }

  static void clearSlot(Slot &slot) {
    slot = {};
  }
};

static_assert(sizeof(AdminQueryWindow) <= 512,
              "Admin query correlation window must remain small and bounded");

}  // namespace mt::admin
