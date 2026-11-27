// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "StoreForwardSettings.h"
#include "../SettingsJournal.h"

namespace mt::modules {

// Two-slot Store & Forward settings journal. It touches only mtsf/state0 and
// mtsf/state1. Missing keys mean raw-zero settings; load never migrates,
// writes, deletes, or aliases any legacy/core/module/identity key.
class StoreForwardJournal {
public:
  static constexpr size_t RecordSize = 40;
  static constexpr size_t HeaderSize = 12;
  enum class State : uint8_t {
    Unloaded,
    Fresh,
    Loaded,
    Recovered,
    Invalid,
    IoError,
  };

private:
  StoreForwardSettings config_{};
  uint32_t generation_ = 0;
  int8_t active_ = -1;
  State state_ = State::Unloaded;

  static const char *key(unsigned slot) {
    return slot ? "mtsf/state1" : "mtsf/state0";
  }
  static uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
  }
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static void put16(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value);
    p[1] = uint8_t(value >> 8);
  }
  static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
  }
  static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
    }
    return ~crc;
  }
  static bool encode(const StoreForwardSettings &config, uint32_t generation,
                     uint8_t *out) {
    if (!config.valid()) return false;
    memset(out, 0, RecordSize);
    out[0] = 'Z';
    out[1] = 'T';
    out[2] = 'S';
    out[3] = 1;
    put32(out + 4, generation);
    put16(out + 8, StoreForwardSettings::Size);
    if (!config.encode(out + HeaderSize, StoreForwardSettings::Size))
      return false;
    put32(out + RecordSize - 4, crc32(out, RecordSize - 4));
    return true;
  }
  static bool decode(const uint8_t *input, int length,
                     StoreForwardSettings &config, uint32_t &generation) {
    if (length != int(RecordSize) || input[0] != 'Z' || input[1] != 'T' ||
        input[2] != 'S' || input[3] != 1 ||
        get16(input + 8) != StoreForwardSettings::Size || input[10] ||
        input[11] ||
        get32(input + RecordSize - 4) != crc32(input, RecordSize - 4))
      return false;
    StoreForwardSettings candidate;
    if (!StoreForwardSettings::decode(input + HeaderSize,
                                      StoreForwardSettings::Size, candidate))
      return false;
    config = candidate;
    generation = get32(input + 4);
    return true;
  }

public:
  const StoreForwardSettings &config() const { return config_; }
  State state() const { return state_; }
  uint32_t generation() const { return generation_; }
  int activeSlot() const { return active_; }
  bool ready() const {
    return state_ == State::Fresh || state_ == State::Loaded ||
           state_ == State::Recovered;
  }

  int load(SettingsBackend &backend) {
    config_ = StoreForwardSettings{};
    generation_ = 0;
    active_ = -1;
    state_ = State::Unloaded;
    uint8_t raw[RecordSize]{};
    StoreForwardSettings candidates[2];
    uint32_t generations[2]{};
    bool good[2]{};
    bool present[2]{};
    for (unsigned slot = 0; slot < 2; ++slot) {
      const int length = backend.read(key(slot), raw, sizeof(raw));
      if (length < 0 && length != -ENOENT && length != -E2BIG) {
        state_ = State::IoError;
        return length;
      }
      present[slot] = length != -ENOENT;
      good[slot] = decode(raw, length, candidates[slot], generations[slot]);
    }
    if (good[0] || good[1]) {
      unsigned selected = good[1] ? 1 : 0;
      if (good[0] && good[1]) {
        const uint32_t delta = generations[1] - generations[0];
        if ((!delta && !candidates[0].same(candidates[1])) ||
            delta == 0x80000000u) {
          state_ = State::Invalid;
          return -EINVAL;
        }
        selected = delta && delta < 0x80000000u ? 1 : 0;
      }
      config_ = candidates[selected];
      generation_ = generations[selected];
      active_ = int8_t(selected);
      state_ = (present[0] && !good[0]) || (present[1] && !good[1])
                   ? State::Recovered
                   : State::Loaded;
      return 0;
    }
    if (!present[0] && !present[1]) {
      state_ = State::Fresh;
      return 0;
    }
    state_ = State::Invalid;
    return -EINVAL;
  }

  int save(SettingsBackend &backend, const StoreForwardSettings &candidate) {
    if (!ready()) return -EACCES;
    if (!candidate.valid()) return -EINVAL;
    if (state_ == State::Loaded && config_.same(candidate)) return 0;

    const unsigned slot = active_ == 0 ? 1 : 0;
    const uint32_t nextGeneration = generation_ + 1;
    uint8_t record[RecordSize];
    uint8_t readback[RecordSize];
    if (!encode(candidate, nextGeneration, record)) return -EINVAL;
    const int result = backend.writeVerified(key(slot), record, sizeof(record), readback, sizeof(readback));
    if (result) {
      state_ = State::IoError;
      return result;
    }
    config_ = candidate;
    generation_ = nextGeneration;
    active_ = int8_t(slot);
    state_ = State::Loaded;
    return 0;
  }
};

static_assert(StoreForwardJournal::RecordSize == 12 + 24 + 4);

}  // namespace mt::modules
