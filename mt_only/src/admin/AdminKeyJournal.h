// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "AdminKeySettings.h"
#include "../SettingsJournal.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>

namespace mt::admin {

// Separate, strict two-slot journal for the public Admin allowlist. Unlike
// recoverable user settings, any present invalid slot revokes the effective
// allowlist until an explicit successful reload/repair.
class AdminKeyJournal {
 public:
  static constexpr size_t HeaderSize = 12;
  static constexpr size_t CrcSize = 4;
  static constexpr size_t RecordSize = HeaderSize + AdminKeySettings::PackedBytes + CrcSize;

  enum class State : uint8_t { Unloaded, Fresh, Loaded, Invalid, IoError };

  const AdminKeySettings &settings() const { return settings_; }
  State state() const { return state_; }
  uint32_t generation() const { return generation_; }
  int activeSlot() const { return active_; }
  bool ready() const { return state_ == State::Fresh || state_ == State::Loaded; }

  bool project(meshtastic_Config_SecurityConfig &out) const {
    if (!ready()) {
      out.admin_key_count = 0;
      memset(out.admin_key, 0, sizeof(out.admin_key));
      return false;
    }
    return settings_.project(out);
  }

  int load(SettingsBackend &backend) {
    poison(State::Unloaded);
    uint8_t raw[RecordSize]{};
    AdminKeySettings candidates[2];
    uint32_t generations[2]{};
    bool present[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
      memset(raw, 0, sizeof(raw));
      const int length = backend.read(key(slot), raw, sizeof(raw));
      if (length == -ENOENT) continue;
      if (length == -E2BIG) {
        poison(State::Invalid);
        return -EINVAL;
      }
      if (length < 0) {
        poison(State::IoError);
        return length;
      }
      present[slot] = true;
      if (!decode(raw, length, candidates[slot], generations[slot])) {
        poison(State::Invalid);
        return -EINVAL;
      }
    }

    if (!present[0] && !present[1]) {
      settings_.clear();
      generation_ = 0;
      active_ = -1;
      state_ = State::Fresh;
      return 0;
    }

    unsigned selected = present[1] ? 1u : 0u;
    if (present[0] && present[1]) {
      const uint32_t delta = generations[1] - generations[0];
      if (delta == 0x80000000u ||
          (delta == 0 && !candidates[0].same(candidates[1]))) {
        poison(State::Invalid);
        return -EINVAL;
      }
      selected = delta && delta < 0x80000000u ? 1u : 0u;
    }

    settings_ = candidates[selected];
    generation_ = generations[selected];
    active_ = static_cast<int8_t>(selected);
    state_ = State::Loaded;
    return 0;
  }

  int save(SettingsBackend &backend, const AdminKeySettings &candidate) {
    if (!ready()) return -EACCES;
    if (!candidate.valid()) return -EINVAL;
    if (settings_.same(candidate)) return 0;

    const unsigned slot = active_ == 0 ? 1u : 0u;
    const uint32_t nextGeneration = generation_ + 1;
    uint8_t record[RecordSize]{}, readback[RecordSize]{};
    encode(candidate, nextGeneration, record);

    const int result = backend.writeVerified(key(slot), record, sizeof(record), readback, sizeof(readback));
    if (result) {
      poison(State::IoError);
      return result;
    }

    settings_ = candidate;
    generation_ = nextGeneration;
    active_ = static_cast<int8_t>(slot);
    state_ = State::Loaded;
    return 0;
  }

 private:
  AdminKeySettings settings_{};
  uint32_t generation_ = 0;
  int8_t active_ = -1;
  State state_ = State::Unloaded;

  static const char *key(unsigned slot) {
    return slot ? "mtadmin/state1" : "mtadmin/state0";
  }

  static uint32_t get32(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
  }

  static void put32(uint8_t *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
  }

  static uint32_t crc32(const uint8_t *bytes, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
      crc ^= bytes[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
    }
    return ~crc;
  }

  static void encode(const AdminKeySettings &settings, uint32_t generation,
                     uint8_t out[RecordSize]) {
    memset(out, 0, RecordSize);
    out[0] = 'Z'; out[1] = 'T'; out[2] = 'A'; out[3] = 1;
    put32(out + 4, generation);
    out[8] = settings.count();
    memcpy(out + HeaderSize, settings.packedKeys(), AdminKeySettings::PackedBytes);
    put32(out + RecordSize - CrcSize, crc32(out, RecordSize - CrcSize));
  }

  static bool decode(const uint8_t *raw, int length, AdminKeySettings &settings,
                     uint32_t &generation) {
    if (length != static_cast<int>(RecordSize) || raw[0] != 'Z' ||
        raw[1] != 'T' || raw[2] != 'A' || raw[3] != 1 || raw[8] > AdminKeySettings::MaximumKeys ||
        raw[9] || raw[10] || raw[11] ||
        get32(raw + RecordSize - CrcSize) != crc32(raw, RecordSize - CrcSize))
      return false;

    AdminKeySettings candidate;
    const size_t count = raw[8];
    const uint8_t *storedKeys = raw + HeaderSize;
    if (!candidate.assign(count, storedKeys, count * AdminKeySettings::KeyBytes) ||
        memcmp(candidate.packedKeys(), storedKeys, AdminKeySettings::PackedBytes))
      return false;
    settings = candidate;
    generation = get32(raw + 4);
    return true;
  }

  void poison(State state) {
    settings_.clear();
    generation_ = 0;
    active_ = -1;
    state_ = state;
  }
};

static_assert(AdminKeyJournal::RecordSize == 112,
              "Admin allowlist record format must remain fixed and bounded");

}  // namespace mt::admin
