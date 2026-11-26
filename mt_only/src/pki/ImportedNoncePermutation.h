// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MtCodec.h"
#include "monocypher.h"

namespace mt::pki {

// NIST SP 800-38G FF1.Encrypt, fixed to four radix-256 digits (a big-endian uint32),
// AES-256 and an empty tweak. The import key belongs to one installation.
// The caller durably reserves the counter before use and handles exhaustion.
class ImportedNoncePermutation {
public:
  static bool apply(const uint8_t key[32], uint32_t counter, uint32_t &out) {
    // P encodes radix 256, 10 rounds, u=2, n=4 and tweak length 0.
    static constexpr uint8_t parameters[16] = {
      1, 2, 1, 0, 1, 0, 10, 2, 0, 0, 0, 4, 0, 0, 0, 0
    };
    struct Scratch {
      uint8_t prefix[16]{}, input[16]{}, encrypted[16]{};
      ~Scratch() { crypto_wipe(this, sizeof(*this)); }
    } scratch;
    // Encrypting a single block of zeros in CTR mode yields AES_K(IV).
    if (!bench::mtAesCtr(nullptr, key, 32, parameters, scratch.prefix, 16)) return false;
    uint16_t left = static_cast<uint16_t>(counter >> 16);
    uint16_t right = static_cast<uint16_t>(counter);
    for (unsigned round = 0; round < 10; ++round) {
      // PRF(P || Q) = AES_K(AES_K(P) xor Q). Q's first 13 bytes are zero.
      memcpy(scratch.input, scratch.prefix, 16);
      scratch.input[13] ^= static_cast<uint8_t>(round);
      scratch.input[14] ^= static_cast<uint8_t>(right >> 8);
      scratch.input[15] ^= static_cast<uint8_t>(right);
      memset(scratch.encrypted, 0, 16);
      if (!bench::mtAesCtr(nullptr, key, 32, scratch.input, scratch.encrypted, 16)) return false;
      // For b=2, d=8. Only the low 16 bits of the first eight bytes of R
      // contribute to addition modulo 65536.
      const unsigned addend = (unsigned(scratch.encrypted[6]) << 8) | scratch.encrypted[7];
      const uint16_t next = static_cast<uint16_t>(left + addend);
      left = right;
      right = next;
    }
    out = (uint32_t(left) << 16) | right;
    return true;
  }
};

} // namespace mt::pki
