// SPDX-License-Identifier: GPL-3.0-only
// NodeInfoPayloadCache.h
//
// Dependency-light, header-only C++17 helper that caches opaque NodeInfo
// payloads keyed by node id. Fixed storage (kCapacity entries, each up to
// kMaxPayload bytes), no dynamic allocation, and no decoded-field, identity,
// signature, RF, or response logic.
//
// observe() records a caller-confirmed genuine NodeInfo frame. The helper
// performs no authentication and cannot verify radio authority; it only stores
// opaque bytes plus a caller-supplied monotonic millisecond timestamp.
//
// Age semantics use full uint64 monotonic milliseconds. A payload is fresh
// when (nowMs - observedMs) <= kMaxAgeMs and observedMs <= nowMs; it is stale
// when the age exceeds kMaxAgeMs, and a future observation (observedMs >
// nowMs) is refused. UINT64_MAX is a valid timestamp; no sentinel values are
// used.
//
// Not thread-safe.

#ifndef MT_TRAFFIC_NODEINFO_PAYLOAD_CACHE_H_
#define MT_TRAFFIC_NODEINFO_PAYLOAD_CACHE_H_

#include <stdint.h>

namespace mt {
namespace traffic {

class NodeInfoPayloadCache {
 public:
  static constexpr uint16_t kCapacity = 8;
  static constexpr uint16_t kMaxPayload = 233;
  static constexpr uint64_t kMaxAgeMs = 21600000ULL;  // 6 hours in ms

  // Record a genuine NodeInfo frame for nodeId at observedMs.
  // Returns false (with no mutation) when nodeId==0, payload==nullptr,
  // size==0, size>kMaxPayload, an existing id's clock rolled back
  // (observedMs < stored time), or a new id has neither a free slot nor a
  // stale slot (age > kMaxAgeMs at observedMs) to replace.
  // Re-observing an existing id replaces its bytes and time; identical bytes
  // from a later genuine frame refresh the full uint64 timestamp.
  bool observe(uint32_t nodeId, const uint8_t *payload, uint16_t size,
              uint64_t observedMs);

  // Copy a fresh payload for nodeId into out (capacity outCapacity), setting
  // outSize. Never refreshes age. On false, out and outSize are left
  // unchanged.
  bool copyFresh(uint32_t nodeId, uint64_t nowMs, uint8_t *out,
                 uint16_t outCapacity, uint16_t &outSize) const;

  // Remove exactly nodeId. Returns true if it existed, false otherwise.
  bool forget(uint32_t nodeId);

 private:
  int findIndex(uint32_t nodeId) const;
  static bool isStaleAt(uint64_t entryObservedMs, uint64_t atMs);

  bool inUse_[kCapacity] = {false};
  uint32_t nodeIds_[kCapacity] = {0};
  uint16_t sizes_[kCapacity] = {0};
  uint64_t observedMs_[kCapacity] = {0};
  uint8_t payloads_[kCapacity][kMaxPayload] = {};
};

inline int NodeInfoPayloadCache::findIndex(uint32_t nodeId) const {
  for (uint16_t i = 0; i < kCapacity; ++i) {
    if (inUse_[i] && nodeIds_[i] == nodeId) return static_cast<int>(i);
  }
  return -1;
}

// Stale at time atMs only when the entry was observed at or before atMs and
// its age then exceeds kMaxAgeMs. A future entry (entryObservedMs > atMs) is
// never stale.
inline bool NodeInfoPayloadCache::isStaleAt(uint64_t entryObservedMs,
                                            uint64_t atMs) {
  if (entryObservedMs > atMs) return false;
  return (atMs - entryObservedMs) > kMaxAgeMs;
}

inline bool NodeInfoPayloadCache::observe(uint32_t nodeId,
                                          const uint8_t *payload,
                                          uint16_t size,
                                          uint64_t observedMs) {
  // Validate first; no mutation on any refusal.
  if (nodeId == 0) return false;
  if (payload == nullptr) return false;
  if (size == 0) return false;
  if (size > kMaxPayload) return false;

  int idx = findIndex(nodeId);
  if (idx >= 0) {
    // Existing id: reject a per-id clock rollback, otherwise replace.
    if (observedMs < observedMs_[idx]) return false;
    for (uint16_t i = 0; i < size; ++i) payloads_[idx][i] = payload[i];
    sizes_[idx] = size;
    observedMs_[idx] = observedMs;
    return true;
  }

  // New id: prefer a free slot; otherwise replace only a stale entry.
  int slot = -1;
  for (uint16_t i = 0; i < kCapacity; ++i) {
    if (!inUse_[i]) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    for (uint16_t i = 0; i < kCapacity; ++i) {
      if (inUse_[i] && isStaleAt(observedMs_[i], observedMs)) {
        slot = static_cast<int>(i);
        break;
      }
    }
  }
  if (slot < 0) return false;  // no free or stale slot: unchanged

  inUse_[slot] = true;
  nodeIds_[slot] = nodeId;
  sizes_[slot] = size;
  observedMs_[slot] = observedMs;
  for (uint16_t i = 0; i < size; ++i) payloads_[slot][i] = payload[i];
  return true;
}

inline bool NodeInfoPayloadCache::copyFresh(uint32_t nodeId, uint64_t nowMs,
                                            uint8_t *out,
                                            uint16_t outCapacity,
                                            uint16_t &outSize) const {
  if (nodeId == 0) return false;
  int idx = findIndex(nodeId);
  if (idx < 0) return false;
  const uint64_t obs = observedMs_[idx];
  if (obs > nowMs) return false;  // future observation: refuse
  if ((nowMs - obs) > kMaxAgeMs) return false;  // stale
  const uint16_t sz = sizes_[idx];
  if (out == nullptr) return false;
  if (outCapacity < sz) return false;
  // All validation passed; mutate only now. No overflow, no partial write.
  for (uint16_t i = 0; i < sz; ++i) out[i] = payloads_[idx][i];
  outSize = sz;
  return true;
}

inline bool NodeInfoPayloadCache::forget(uint32_t nodeId) {
  if (nodeId == 0) return false;
  int idx = findIndex(nodeId);
  if (idx < 0) return false;
  inUse_[idx] = false;
  return true;
}

}  // namespace traffic
}  // namespace mt

#endif  // MT_TRAFFIC_NODEINFO_PAYLOAD_CACHE_H_
