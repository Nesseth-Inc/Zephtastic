// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "meshtastic/module_config.pb.h"

namespace mt::modules {

// Independent raw Store & Forward module settings. Zero is retained exactly
// for every field; runtime defaults are applied only by normalized().
struct StoreForwardSettings {
  static constexpr size_t Size = 24;
  static constexpr uint8_t Version = 1;
  static constexpr uint32_t MaxRecords = 400;
  static constexpr uint32_t MaxReturnMax = 400;
  // Control computes minutes * 60, then seconds * 1000 in uint32_t fields.
  static constexpr uint32_t MaxReturnWindowMinutes = UINT32_MAX / 60000u;
  static constexpr uint32_t DefaultRecords = 400;
  static constexpr uint32_t DefaultReturnMax = 25;
  static constexpr uint32_t DefaultReturnWindowMinutes = 240;

  struct RuntimeValues {
    bool enabled;
    bool heartbeat;
    bool isServer;
    uint32_t records;
    uint32_t historyReturnMax;
    uint32_t historyReturnWindow;
  };

private:
  uint8_t bytes_[Size]{};

  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
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
  static bool valuesValid(const meshtastic_ModuleConfig_StoreForwardConfig &v) {
    return v.records <= MaxRecords &&
           (!v.history_return_max || v.history_return_max <= MaxReturnMax) &&
           v.history_return_window <= MaxReturnWindowMinutes;
  }
  void seal() { put32(bytes_ + 20, crc32(bytes_, 20)); }

public:
  StoreForwardSettings() {
    bytes_[0] = 'Z';
    bytes_[1] = 'T';
    bytes_[2] = 'F';
    bytes_[3] = Version;
    seal();
  }

  bool valid() const {
    if (bytes_[0] != 'Z' || bytes_[1] != 'T' || bytes_[2] != 'F' ||
        bytes_[3] != Version || (bytes_[4] & ~uint8_t(7)) || bytes_[5] ||
        bytes_[6] || bytes_[7] || get32(bytes_ + 20) != crc32(bytes_, 20))
      return false;
    const uint32_t records = get32(bytes_ + 8);
    const uint32_t returnMax = get32(bytes_ + 12);
    const uint32_t returnWindow = get32(bytes_ + 16);
    return records <= MaxRecords &&
           (!returnMax || returnMax <= MaxReturnMax) &&
           returnWindow <= MaxReturnWindowMinutes;
  }

  bool same(const StoreForwardSettings &other) const {
    return memcmp(bytes_, other.bytes_, Size) == 0;
  }

  bool set(const meshtastic_ModuleConfig_StoreForwardConfig &value) {
    if (!valuesValid(value)) return false;
    StoreForwardSettings candidate;
    candidate.bytes_[0] = 'Z';
    candidate.bytes_[1] = 'T';
    candidate.bytes_[2] = 'F';
    candidate.bytes_[3] = Version;
    candidate.bytes_[4] = uint8_t((value.enabled ? 1 : 0) |
                                  (value.heartbeat ? 2 : 0) |
                                  (value.is_server ? 4 : 0));
    put32(candidate.bytes_ + 8, value.records);
    put32(candidate.bytes_ + 12, value.history_return_max);
    put32(candidate.bytes_ + 16, value.history_return_window);
    candidate.seal();
    if (!candidate.valid()) return false;
    *this = candidate;
    return true;
  }

  bool get(meshtastic_ModuleConfig_StoreForwardConfig &out) const {
    out = meshtastic_ModuleConfig_StoreForwardConfig_init_zero;
    if (!valid()) return false;
    out.enabled = (bytes_[4] & 1) != 0;
    out.heartbeat = (bytes_[4] & 2) != 0;
    out.is_server = (bytes_[4] & 4) != 0;
    out.records = get32(bytes_ + 8);
    out.history_return_max = get32(bytes_ + 12);
    out.history_return_window = get32(bytes_ + 16);
    return true;
  }

  RuntimeValues normalized() const {
    meshtastic_ModuleConfig_StoreForwardConfig raw{};
    (void)get(raw);
    return RuntimeValues{
        raw.enabled, raw.heartbeat, raw.is_server,
        raw.records ? raw.records : DefaultRecords,
        raw.history_return_max ? raw.history_return_max : DefaultReturnMax,
        raw.history_return_window ? raw.history_return_window
                                  : DefaultReturnWindowMinutes};
  }

  bool encode(uint8_t *out, size_t capacity) const {
    if (!out || capacity < Size || !valid()) return false;
    memcpy(out, bytes_, Size);
    return true;
  }

  static bool decode(const uint8_t *input, size_t length,
                     StoreForwardSettings &out) {
    if (!input || length != Size) return false;
    StoreForwardSettings candidate;
    memcpy(candidate.bytes_, input, Size);
    if (!candidate.valid()) return false;
    out = candidate;
    return true;
  }
};

static_assert(StoreForwardSettings::Size == 24);
static_assert(StoreForwardSettings::MaxReturnWindowMinutes == 71582);
static_assert(sizeof(StoreForwardSettings) == StoreForwardSettings::Size);

}  // namespace mt::modules
