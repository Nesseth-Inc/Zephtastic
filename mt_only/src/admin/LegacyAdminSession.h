// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mt::admin {

// Volatile stock-compatible session passkey for the opt-in named shared-PSK
// Admin channel. This state is independent of private-PKI peer sessions.
class LegacyAdminSession {
public:
  static constexpr size_t TokenBytes = 8;
  static constexpr uint32_t RotateAfterMs = 150000u;
  static constexpr uint32_t ValidForMs = 300000u;
  using Entropy = bool (*)(void *context, uint8_t *out, size_t size);

  LegacyAdminSession() = default;
  LegacyAdminSession(const LegacyAdminSession &) = delete;
  LegacyAdminSession &operator=(const LegacyAdminSession &) = delete;
  ~LegacyAdminSession() { reset(); }

  // Current token is reused before 150 seconds; afterward a fresh nonzero
  // token is required. Failure clears prior state and output.
  bool issue(uint32_t now, Entropy entropy, void *context,
             uint8_t out[TokenBytes]) {
    if (!out) { reset(); return false; }
    wipe(out, TokenBytes);
    if (!entropy) { reset(); return false; }
    if (valid_ && uint32_t(now - issuedAt_) < RotateAfterMs) {
      copy(out, token_, TokenBytes);
      return true;
    }
    uint8_t candidate[TokenBytes]{};
    const bool filled = entropy(context, candidate, sizeof(candidate));
    uint8_t aggregate = 0;
    for (size_t i = 0; i < sizeof(candidate); ++i)
      aggregate = uint8_t(aggregate | candidate[i]);
    const bool changed = !valid_ || !equal(candidate, token_, sizeof(candidate));
    if (!filled || !aggregate || !changed) {
      wipe(candidate, sizeof(candidate)); reset(); return false;
    }
    wipe(token_, sizeof(token_));
    copy(token_, candidate, sizeof(token_));
    issuedAt_ = now; valid_ = true;
    copy(out, token_, TokenBytes);
    wipe(candidate, sizeof(candidate));
    return true;
  }

  // Called on every owner poll, including idle periods, so an old token
  // cannot become valid again after a long gap spanning uint32 rollover.
  void poll(uint32_t now) {
    if (valid_ && uint32_t(now - issuedAt_) >= ValidForMs) reset();
  }

  bool validate(const uint8_t *candidate, size_t size, uint32_t now) {
    poll(now);
    return valid_ && candidate && size == TokenBytes &&
           equal(candidate, token_, TokenBytes);
  }

  const uint8_t *token() const { return valid_ ? token_ : nullptr; }
  bool valid() const { return valid_; }

  void reset() {
    wipe(token_, sizeof(token_));
    issuedAt_ = 0; valid_ = false;
  }

private:
  static void wipe(void *memory, size_t size) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(memory);
    while (size--) *p++ = 0;
  }
  static void copy(uint8_t *out, const uint8_t *in, size_t size) {
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
  }
  static bool equal(const uint8_t *a, const uint8_t *b, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i)
      difference = uint8_t(difference | uint8_t(a[i] ^ b[i]));
    return difference == 0;
  }
  uint8_t token_[TokenBytes]{};
  uint32_t issuedAt_ = 0;
  bool valid_ = false;
};

}  // namespace mt::admin
