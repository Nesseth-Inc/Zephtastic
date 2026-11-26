// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../SettingsJournal.h"
#include "RemoteHardwarePinCache.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

namespace mt::admin {

// Strict two-slot journal for descriptive remote pin metadata only. It is
// scoped to one current public identity domain and grants no GPIO authority.
class RemoteHardwarePinJournal {
public:
  static constexpr size_t Capacity = RemoteHardwarePinCache::Capacity;
  static constexpr size_t HeaderSize = 48;
  static constexpr size_t PinBytes = 21; // node32, gpio8, type8, name[15]
  static constexpr size_t CrcSize = 4;
  static constexpr size_t RecordSize = HeaderSize + Capacity * PinBytes + CrcSize;
  using Entry = RemoteHardwarePinCache::Entry;

  struct Domain {
    uint32_t node = 0;
    uint8_t publicKey[32]{};
    bool valid() const {
      if (node <= 1 || node >= 0xfffffffeu) return false;
      uint8_t any = 0;
      for (uint8_t byte : publicKey) any |= byte;
      return any != 0;
    }
  };

  struct Snapshot {
    size_t count = 0;
    Entry entries[Capacity]{};
  };

  enum class State : uint8_t { Unloaded, Fresh, Loaded, Invalid, IoError };

private:
  Snapshot snapshot_{};
  Domain domain_{};
  uint32_t generation_ = 0;
  int8_t active_ = -1;
  State state_ = State::Unloaded;

  static const char *key(unsigned slot) {
    return slot ? "mtpin/state1" : "mtpin/state0";
  }

  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
  }
  static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
    }
    return ~crc;
  }
  static bool validNode(uint32_t node) { return node > 1 && node < 0xfffffffeu; }
  static uint32_t pinTypeRaw(const meshtastic_RemoteHardwarePin &pin) {
    using RawType = __underlying_type(meshtastic_RemoteHardwarePinType);
    RawType type{};
    memcpy(&type, &pin.type, sizeof(type));
    return static_cast<uint32_t>(type);
  }
  static bool validPin(const meshtastic_RemoteHardwarePin &pin) {
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
  static bool validSnapshot(const Snapshot &snapshot) {
    if (snapshot.count > Capacity) return false;
    size_t perPeer[Capacity]{};
    uint32_t peers[Capacity]{};
    size_t peerCount = 0;
    for (size_t i = 0; i < snapshot.count; ++i) {
      const Entry &entry = snapshot.entries[i];
      if (!validNode(entry.node_num) || !entry.has_pin || !validPin(entry.pin)) return false;
      size_t peer = 0;
      while (peer < peerCount && peers[peer] != entry.node_num) ++peer;
      if (peer == peerCount) {
        peers[peerCount] = entry.node_num;
        perPeer[peerCount] = 0;
        ++peerCount;
      }
      if (++perPeer[peer] > RemoteHardwarePinCache::PeerCapacity) return false;
      for (size_t prior = 0; prior < i; ++prior)
        if (snapshot.entries[prior].node_num == entry.node_num &&
            snapshot.entries[prior].pin.gpio_pin == entry.pin.gpio_pin) return false;
    }
    for (size_t i = snapshot.count; i < Capacity; ++i) {
      const Entry &entry = snapshot.entries[i];
      if (entry.node_num || entry.has_pin || entry.pin.gpio_pin ||
          pinTypeRaw(entry.pin) != meshtastic_RemoteHardwarePinType_UNKNOWN) return false;
      for (char byte : entry.pin.name) if (byte) return false;
    }
    return true;
  }
  static bool same(const Snapshot &a, const Snapshot &b) {
    if (a.count != b.count) return false;
    for (size_t i = 0; i < a.count; ++i) {
      const Entry &x = a.entries[i], &y = b.entries[i];
      if (x.node_num != y.node_num || x.has_pin != y.has_pin ||
          x.pin.gpio_pin != y.pin.gpio_pin || pinTypeRaw(x.pin) != pinTypeRaw(y.pin) ||
          memcmp(x.pin.name, y.pin.name, sizeof(x.pin.name))) return false;
    }
    return true;
  }
  static bool sameDomain(const Domain &a, const Domain &b) {
    return a.node == b.node && !memcmp(a.publicKey, b.publicKey, sizeof(a.publicKey));
  }
  static void encode(const Snapshot &snapshot, const Domain &domain,
                     uint32_t generation, uint8_t out[RecordSize]) {
    memset(out, 0, RecordSize);
    out[0] = 'Z'; out[1] = 'T'; out[2] = 'P'; out[3] = 'N';
    out[4] = 1; out[5] = uint8_t(snapshot.count);
    put32(out + 8, generation);
    put32(out + 12, domain.node);
    memcpy(out + 16, domain.publicKey, sizeof(domain.publicKey));
    for (size_t i = 0; i < snapshot.count; ++i) {
      uint8_t *p = out + HeaderSize + i * PinBytes;
      const Entry &entry = snapshot.entries[i];
      put32(p, entry.node_num);
      p[4] = entry.pin.gpio_pin;
      p[5] = pinTypeRaw(entry.pin);
      memcpy(p + 6, entry.pin.name, sizeof(entry.pin.name));
    }
    put32(out + RecordSize - CrcSize, crc32(out, RecordSize - CrcSize));
  }
  static bool decode(const uint8_t *raw, int length, const Domain &domain,
                     Snapshot &snapshot, uint32_t &generation) {
    if (length != static_cast<int>(RecordSize) || raw[0] != 'Z' || raw[1] != 'T' ||
        raw[2] != 'P' || raw[3] != 'N' || raw[4] != 1 || raw[5] > Capacity ||
        raw[6] || raw[7] || get32(raw + 12) != domain.node ||
        memcmp(raw + 16, domain.publicKey, sizeof(domain.publicKey)) ||
        get32(raw + RecordSize - CrcSize) != crc32(raw, RecordSize - CrcSize)) return false;
    Snapshot candidate{};
    candidate.count = raw[5];
    for (size_t i = 0; i < Capacity; ++i) {
      const uint8_t *p = raw + HeaderSize + i * PinBytes;
      if (i >= candidate.count) {
        for (size_t j = 0; j < PinBytes; ++j) if (p[j]) return false;
        continue;
      }
      Entry &entry = candidate.entries[i];
      entry.node_num = get32(p);
      entry.has_pin = true;
      entry.pin.gpio_pin = p[4];
      if (p[5] > meshtastic_RemoteHardwarePinType_DIGITAL_WRITE) return false;
      entry.pin.type = static_cast<meshtastic_RemoteHardwarePinType>(p[5]);
      memcpy(entry.pin.name, p + 6, sizeof(entry.pin.name));
    }
    if (!validSnapshot(candidate)) return false;
    snapshot = candidate;
    generation = get32(raw + 8);
    return true;
  }
  void poison(State state) {
    snapshot_ = Snapshot{};
    generation_ = 0;
    active_ = -1;
    state_ = state;
  }

public:
  const Snapshot &snapshot() const { return snapshot_; }
  const Domain &domain() const { return domain_; }
  State state() const { return state_; }
  uint32_t generation() const { return generation_; }
  int activeSlot() const { return active_; }
  bool ready() const { return state_ == State::Fresh || state_ == State::Loaded; }

  // Missing slots are an empty Fresh journal and cause no write. Any present
  // malformed, wrong-domain, or conflicting slot disables inventory entirely.
  int load(SettingsBackend &backend, const Domain &currentDomain) {
    poison(State::Unloaded);
    domain_ = currentDomain;
    if (!domain_.valid()) { poison(State::Invalid); return -EINVAL; }
    uint8_t raw[RecordSize]{};
    Snapshot candidates[2]{};
    uint32_t generations[2]{};
    bool present[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
      memset(raw, 0, sizeof(raw));
      const int length = backend.read(key(slot), raw, sizeof(raw));
      if (length == -ENOENT) continue;
      if (length == -E2BIG) { poison(State::Invalid); return -EINVAL; }
      if (length < 0) { poison(State::IoError); return length; }
      present[slot] = true;
      if (!decode(raw, length, domain_, candidates[slot], generations[slot])) {
        poison(State::Invalid);
        return -EINVAL;
      }
    }
    if (!present[0] && !present[1]) {
      snapshot_ = Snapshot{};
      generation_ = 0;
      active_ = -1;
      state_ = State::Fresh;
      return 0;
    }

    unsigned selected = present[1] ? 1u : 0u;
    if (present[0] && present[1]) {
      const uint32_t delta = generations[1] - generations[0];
      if (delta == 0x80000000u || (delta == 0 && !same(candidates[0], candidates[1]))) {
        poison(State::Invalid);
        return -EINVAL;
      }
      selected = delta && delta < 0x80000000u ? 1u : 0u;
    }
    snapshot_ = candidates[selected];
    generation_ = generations[selected];
    active_ = static_cast<int8_t>(selected);
    state_ = State::Loaded;
    return 0;
  }

  int save(SettingsBackend &backend, const Snapshot &candidate) {
    if (!ready()) return -EACCES;
    if (!validSnapshot(candidate)) return -EINVAL;
    if (same(snapshot_, candidate)) return 0;

    const unsigned slot = active_ == 0 ? 1u : 0u;
    const uint32_t nextGeneration = generation_ + 1;
    uint8_t record[RecordSize]{}, readback[RecordSize]{};
    encode(candidate, domain_, nextGeneration, record);
    const int written = backend.write(key(slot), record, sizeof(record));
    if (written) { poison(State::IoError); return written; }

    const int length = backend.read(key(slot), readback, sizeof(readback));
    Snapshot observed{};
    uint32_t observedGeneration = 0;
    if (length != static_cast<int>(sizeof(record)) || memcmp(record, readback, sizeof(record)) ||
        !decode(readback, length, domain_, observed, observedGeneration) ||
        observedGeneration != nextGeneration || !same(observed, candidate)) {
      poison(State::IoError);
      return length < 0 ? length : -EIO;
    }
    snapshot_ = candidate;
    generation_ = nextGeneration;
    active_ = static_cast<int8_t>(slot);
    state_ = State::Loaded;
    return 0;
  }
};

static_assert(RemoteHardwarePinJournal::RecordSize == 304,
              "remote pin journal layout must remain fixed and bounded");
static_assert(RemoteHardwarePinJournal::PinBytes ==
                  6 + sizeof(((meshtastic_RemoteHardwarePin *)nullptr)->name),
              "remote pin journal descriptor format must match the pinned name bound");

} // namespace mt::admin
