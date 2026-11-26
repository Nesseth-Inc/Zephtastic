// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "meshtastic/config.pb.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::admin {

// Public Admin key allowlist only. This type has no identity or peer-pin
// fields, and its persistent representation never contains private keys.
class AdminKeySettings {
 public:
  static constexpr size_t MaximumKeys = 3;
  static constexpr size_t KeyBytes = 32;
  static constexpr size_t PackedBytes = MaximumKeys * KeyBytes;

  uint8_t count() const { return count_; }
  const uint8_t *packedKeys() const { return &keys_[0][0]; }

  const uint8_t *key(size_t index) const {
    return index < count_ ? keys_[index] : nullptr;
  }

  bool valid() const {
    if (count_ > MaximumKeys) return false;
    for (size_t i = 0; i < MaximumKeys; ++i) {
      const bool empty = allZero(keys_[i], KeyBytes);
      if (i < count_ ? empty : !empty) return false;
    }
    return true;
  }

  // Input is packed active keys only. Empty settings are valid and mean
  // deny-all; no backend writes occur until a journal save is requested.
  bool assign(size_t count, const uint8_t *packed, size_t length) {
    if (count > MaximumKeys || length != count * KeyBytes ||
        (length && !packed))
      return false;
    AdminKeySettings candidate;
    candidate.count_ = static_cast<uint8_t>(count);
    if (length) memcpy(candidate.keys_, packed, length);
    if (!candidate.valid()) return false;
    *this = candidate;
    return true;
  }

  bool same(const AdminKeySettings &other) const {
    return count_ == other.count_ &&
           memcmp(keys_, other.keys_, sizeof(keys_)) == 0;
  }

  void clear() {
    count_ = 0;
    memset(keys_, 0, sizeof(keys_));
  }

  // Replace only SecurityConfig's Admin allowlist fields. Other security
  // material and flags in the typed projection remain untouched.
  bool project(meshtastic_Config_SecurityConfig &out) const {
    if (!valid()) {
      out.admin_key_count = 0;
      memset(out.admin_key, 0, sizeof(out.admin_key));
      return false;
    }
    out.admin_key_count = count_;
    memset(out.admin_key, 0, sizeof(out.admin_key));
    for (size_t i = 0; i < count_; ++i) {
      out.admin_key[i].size = KeyBytes;
      memcpy(out.admin_key[i].bytes, keys_[i], KeyBytes);
    }
    return true;
  }

 private:
  uint8_t count_ = 0;
  uint8_t keys_[MaximumKeys][KeyBytes]{};

  static bool allZero(const uint8_t *bytes, size_t length) {
    uint8_t value = 0;
    for (size_t i = 0; i < length; ++i) value |= bytes[i];
    return value == 0;
  }
};

static_assert(sizeof(AdminKeySettings) == 1 + AdminKeySettings::PackedBytes,
              "Admin key settings must remain a fixed packed RAM value");

}  // namespace mt::admin
