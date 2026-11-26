// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "meshtastic/mesh.pb.h"

namespace mt::admin {

// Bounded typed cache of remote GPIO descriptors. Owner calls are serialized;
// the correlation and canonical-wire booleans must come from its validated
// pending private Admin response path. This is descriptive metadata only.
class RemoteHardwarePinCache {
public:
  static constexpr size_t Capacity = 12;
  static constexpr size_t PeerCapacity = 4;
  using Entry = meshtastic_NodeRemoteHardwarePin;

private:
  Entry entries_[Capacity]{};
  size_t count_ = 0;

  static bool validNode(uint32_t node) { return node > 1 && node < 0xfffffffeu; }

  static bool validDescriptor(const meshtastic_RemoteHardwarePin &pin) {
    if (pin.gpio_pin >= 64) return false;
    const size_t length = strnlen(pin.name, sizeof(pin.name));
    if (length == sizeof(pin.name)) return false;
    for (size_t i = length + 1; i < sizeof(pin.name); ++i)
      if (pin.name[i] != 0) return false;
    using RawType = __underlying_type(meshtastic_RemoteHardwarePinType);
    RawType type{};
    memcpy(&type, &pin.type, sizeof(type));
    return type >= static_cast<RawType>(meshtastic_RemoteHardwarePinType_UNKNOWN) &&
           type <= static_cast<RawType>(meshtastic_RemoteHardwarePinType_DIGITAL_WRITE);
  }

  static bool samePin(const Entry &a, const Entry &b) {
    return a.node_num == b.node_num && a.pin.gpio_pin == b.pin.gpio_pin;
  }

public:
  // Atomically replaces all descriptors for node. A valid empty list removes
  // the peer. Failed validation/capacity leaves the old cache unchanged.
  bool updatePeer(uint32_t node, bool correlatedExpectedPeer,
                  bool canonicalWireValidated, const Entry *typedPins,
                  size_t typedPinCount) {
    if (!correlatedExpectedPeer || !canonicalWireValidated || !validNode(node) ||
        typedPinCount > PeerCapacity || (typedPinCount && !typedPins)) return false;

    Entry candidate[PeerCapacity]{};
    for (size_t i = 0; i < typedPinCount; ++i) {
      const Entry &entry = typedPins[i];
      if (entry.node_num != node || !validNode(entry.node_num) || !entry.has_pin ||
          !validDescriptor(entry.pin)) return false;
      candidate[i] = entry;
      for (size_t j = 0; j < i; ++j)
        if (samePin(candidate[j], candidate[i])) return false;
    }

    Entry next[Capacity]{};
    size_t nextCount = 0;
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].node_num == node) continue;
      if (nextCount == Capacity) return false;
      next[nextCount++] = entries_[i];
    }
    if (typedPinCount > Capacity - nextCount) return false;
    for (size_t i = 0; i < typedPinCount; ++i) {
      for (size_t j = 0; j < nextCount; ++j)
        if (samePin(next[j], candidate[i])) return false;
      next[nextCount++] = candidate[i];
    }

    memset(entries_, 0, sizeof(entries_));
    if (nextCount) memcpy(entries_, next, nextCount * sizeof(Entry));
    count_ = nextCount;
    return true;
  }

  // Restores a complete snapshot from a successfully loaded local typed
  // journal. This is metadata provenance, not peer-correlation proof or GPIO
  // permission, and is intentionally separate from the RF updater.
  bool restoreCommittedSnapshot(const Entry *typedEntries, size_t entryCount) {
    if (entryCount > Capacity || (entryCount && !typedEntries)) return false;
    Entry candidate[Capacity]{};
    size_t peerCounts[Capacity]{};
    uint32_t peers[Capacity]{};
    size_t peerCount = 0;
    for (size_t i = 0; i < entryCount; ++i) {
      const Entry &entry = typedEntries[i];
      if (!validNode(entry.node_num) || !entry.has_pin || !validDescriptor(entry.pin)) return false;
      candidate[i] = entry;
      size_t peer = 0;
      while (peer < peerCount && peers[peer] != entry.node_num) ++peer;
      if (peer == peerCount) {
        peers[peerCount] = entry.node_num;
        peerCounts[peerCount] = 0;
        ++peerCount;
      }
      if (++peerCounts[peer] > PeerCapacity) return false;
      for (size_t prior = 0; prior < i; ++prior)
        if (samePin(candidate[prior], candidate[i])) return false;
    }
    memset(entries_, 0, sizeof(entries_));
    if (entryCount) memcpy(entries_, candidate, entryCount * sizeof(Entry));
    count_ = entryCount;
    return true;
  }

  bool removePeer(uint32_t node) {
    if (!validNode(node)) return false;
    Entry next[Capacity]{};
    size_t nextCount = 0;
    for (size_t i = 0; i < count_; ++i)
      if (entries_[i].node_num != node) next[nextCount++] = entries_[i];
    memset(entries_, 0, sizeof(entries_));
    if (nextCount) memcpy(entries_, next, nextCount * sizeof(Entry));
    count_ = nextCount;
    return true;
  }

  // Only clears RAM metadata; this does not erase its journal or any keys.
  void clear() {
    memset(entries_, 0, sizeof(entries_));
    count_ = 0;
  }

  bool copyTo(Entry *out, size_t capacity, size_t &outCount) const {
    outCount = 0;
    if (count_ && (!out || capacity < count_)) return false;
    if (count_) memcpy(out, entries_, count_ * sizeof(Entry));
    outCount = count_;
    return true;
  }

  size_t size() const { return count_; }
  const Entry *data() const { return entries_; }
};

} // namespace mt::admin
