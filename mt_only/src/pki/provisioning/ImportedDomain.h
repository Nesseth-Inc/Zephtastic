// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../PkiFreshRoot.h"
#include "../PkiNonceCounter.h"
#include "MtSignature.h"
#include "monocypher.h"

namespace mt::pki {

// The imported selector and pending record use the existing exclusive storage
// slots. Older firmware rejects both magic values before activating the key.
struct ImportedDomain {
  static constexpr size_t SaltSize = 16;

  static bool hasSalt(const uint8_t salt[SaltSize]) {
    uint8_t aggregate = 0;
    for (size_t i = 0; i < SaltSize; ++i) aggregate |= salt[i];
    return aggregate != 0;
  }

  static bool binding(const uint8_t scalar[32], const uint8_t expected[32],
                      IdentityBinding &out) {
    if (!scalar || !expected || (scalar[0] & 7) || (scalar[31] & 0xc0) != 0x40) return false;

    MonocypherIdentityDeriver deriver;
    uint8_t derived[32]{}, edwards[32]{};
    const bool matches = deriver.derivePublic(scalar, derived) &&
                         crypto_verify32(derived, expected) == 0 &&
                         bench::MtSignature::admissiblePublicKey(derived, edwards);
    const uint32_t node = matches ? bench::MtSignature::nodeId(derived) : 0;
    eraseIdentityBytes(derived, sizeof(derived));
    eraseIdentityBytes(edwards, sizeof(edwards));
    if (node <= 1 || node >= 0xfffffffeu) return false;

    out = {};
    out.generation = 1;
    out.node = node;
    memcpy(out.publicKey, expected, 32);
    return true;
  }

  static bool root(const IdentityBinding &binding, const uint8_t salt[SaltSize],
                   uint8_t out[56], PkiFreshRoot &root) {
    if (!hasSalt(salt) || binding.version != 1 || binding.algorithm != 1 ||
        binding.generation != 1 || binding.node <= 1 || binding.node >= 0xfffffffeu ||
        binding.node != bench::MtSignature::nodeId(binding.publicKey)) return false;

    memcpy(out, "ZTI1", 4);
    memcpy(out + 4, salt, SaltSize);
    memcpy(out + 20, binding.publicKey, 32);
    PkiFreshRoot::put(out + 52, PkiFreshRoot::checksum(out, 52));

    root = {};
    // Contacts carry both root generation and CRC. Both distinguish independent
    // imports, including successive installations of the same public key.
    const uint32_t domainGeneration = PkiFreshRoot::word(salt);
    root.generation = domainGeneration ? domainGeneration : 1;
    root.identityGeneration = binding.generation;
    root.node = binding.node;
    memcpy(root.publicKey, binding.publicKey, 32);
    root.crc = PkiFreshRoot::word(out + 52);
    return true;
  }

  static bool matchesRoot(const uint8_t *encoded, size_t length, const IdentityBinding &owner,
                          PkiFreshRoot &decoded) {
    if (!encoded || length != PkiFreshRoot::RecordBytes || memcmp(encoded, "ZTI1", 4)) return false;

    uint8_t expected[PkiFreshRoot::RecordBytes]{};
    return root(owner, encoded + 4, expected, decoded) && !memcmp(expected, encoded, sizeof(expected));
  }

  static void intent(const IdentityBinding &binding, const uint8_t scalar[32],
                     const uint8_t salt[SaltSize], uint8_t out[84]) {
    memset(out, 0, 84);
    memcpy(out, "ZTP2", 4);
    PkiFreshRoot::put(out + 12, binding.node);
    memcpy(out + 16, scalar, 32);
    memcpy(out + 48, salt, SaltSize);
    PkiFreshRoot::put(out + 80, PkiFreshRoot::checksum(out, 80));
  }

  static bool readIntent(const uint8_t record[84], const IdentityBinding &expected,
                         uint8_t salt[SaltSize]) {
    if (memcmp(record, "ZTP2", 4) ||
        PkiFreshRoot::word(record + 80) != PkiFreshRoot::checksum(record, 80) ||
        PkiFreshRoot::word(record + 12) != expected.node || !hasSalt(record + 48)) return false;
    for (unsigned i = 4; i < 12; ++i) if (record[i]) return false;
    for (unsigned i = 64; i < 80; ++i) if (record[i]) return false;

    IdentityBinding proven;
    if (!binding(record + 16, expected.publicKey, proven) || proven.node != expected.node) return false;
    memcpy(salt, record + 48, SaltSize);
    return true;
  }

  static void keys(const IdentityBinding &owner, const uint8_t scalar[32],
                   const uint8_t salt[SaltSize], PkiNonceCounterBinding &counter,
                   uint8_t permutation[32]) {
    // Bind the counter to this exact installation of the identity. Its header
    // is public, while the independent FF1 key requires the private scalar.
    uint8_t input[64]{};
    memcpy(input, "ZT/import-ctr/v1", 16);
    memcpy(input + 16, salt, SaltSize);
    memcpy(input + 32, owner.publicKey, 32);
    crypto_blake2b(counter.publicKey, 32, input, sizeof(input));

    memcpy(input, "ZT/import-ff1/v1", 16);
    crypto_blake2b_keyed(permutation, 32, scalar, 32, input, sizeof(input));
    eraseIdentityBytes(input, sizeof(input));
  }
};

} // namespace mt::pki
