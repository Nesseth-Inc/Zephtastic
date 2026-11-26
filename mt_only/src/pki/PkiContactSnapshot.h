// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "PkiPeerSet.h"

namespace mt::pki {

// Canonical 640-byte payload: magic[0..3], version[4..5], count[6..7],
// snapshot generation[8..11], local node[12..15], local public key[16..47],
// local identity generation[48..51], legacy root generation[52..55], legacy
// root CRC[56..59], sixteen 36-byte rows[60..635], and IEEE CRC32[636..639].
// Canonical payload proposal for one bounded active-contact snapshot.
// It is a pure codec: it has no storage, selector, activation, replay-domain,
// or lifecycle authority. A caller must bind both the full expected local
// identity and immutable legacy root tuple before using decoded contacts.
class PkiContactSnapshot {
public:
  static constexpr size_t Capacity = PkiPeerSet::Limit;
  static constexpr size_t HeaderBytes = 60;
  static constexpr size_t RowBytes = 36;
  static constexpr size_t RowsBytes = Capacity * RowBytes;
  static constexpr size_t CrcOffset = HeaderBytes + RowsBytes;
  static constexpr size_t RecordBytes = CrcOffset + 4;
  static constexpr uint32_t Magic = 0x31534350u; // "PCS1", little endian
  static constexpr uint16_t Version = 1;

  struct Value {
    uint32_t generation = 0;
    uint32_t localIdentityGeneration = 0;
    uint32_t legacyRootGeneration = 0;
    uint32_t legacyRootCrc = 0;
    uint16_t count = 0;
    PkiPeerPin contacts[Capacity]{};
  };

  static_assert(RecordBytes == 640, "M153 bounded contact snapshot size");

  // Encode only canonical active contacts. Empty snapshots are supported;
  // count>0 requires a non-null array. Rows are sorted by node ascending.
  // `out` is changed only after every semantic check has passed.
  static bool encode(const IdentityBinding &local, uint32_t generation,
                     uint32_t legacyRootGeneration, uint32_t legacyRootCrc,
                     const PkiPeerPin *contacts, size_t count,
                     uint8_t *out, size_t outSize) {
    if (!out || outSize != RecordBytes || generation == 0 ||
        legacyRootGeneration == 0 || count > Capacity ||
        (count != 0 && !contacts)) {
      return false;
    }

    PkiPeerSet validated;
    if (!validated.assignEmpty(local)) return false;
    if (count && !validated.assignTrusted(local, contacts, count)) return false;

    // Stage only the fixed contact rows, then sort them. The larger record is
    // written in place only after validation and ordering can no longer fail.
    PkiPeerPin canonical[Capacity]{};
    for (size_t i = 0; i < count; ++i) {
      const PkiPeerPin *pin = validated.at(i);
      if (!pin) return false;
      canonical[i] = *pin;
    }
    for (size_t i = 1; i < count; ++i) {
      PkiPeerPin item = canonical[i];
      size_t j = i;
      while (j && canonical[j - 1].node > item.node) {
        canonical[j] = canonical[j - 1];
        --j;
      }
      canonical[j] = item;
    }

    const uint32_t localNode = local.node;
    const uint32_t localGeneration = local.generation;
    uint8_t localPublic[32];
    memcpy(localPublic, local.publicKey, sizeof(localPublic));
    memset(out, 0, RecordBytes);
    put32(out, Magic);
    put16(out + 4, Version);
    put16(out + 6, static_cast<uint16_t>(count));
    put32(out + 8, generation);
    put32(out + 12, localNode);
    memcpy(out + 16, localPublic, sizeof(localPublic));
    put32(out + 48, localGeneration);
    put32(out + 52, legacyRootGeneration);
    put32(out + 56, legacyRootCrc);
    for (size_t i = 0; i < count; ++i) {
      uint8_t *row = out + HeaderBytes + i * RowBytes;
      put32(row, canonical[i].node);
      memcpy(row + 4, canonical[i].publicKey, 32);
    }
    put32(out + CrcOffset, crc32(out, CrcOffset));
    return true;
  }

  // Decode against exact current local identity and legacy-root generation/CRC.
  // Active rows must be strictly ascending by node. Output is unchanged on
  // every failure, including CRC-valid semantic failures late in the record.
  static bool decode(const uint8_t *input, size_t inputSize,
                     const IdentityBinding &expectedLocal,
                     uint32_t expectedLegacyRootGeneration,
                     uint32_t expectedLegacyRootCrc, Value &out) {
    if (!input || inputSize != RecordBytes ||
        expectedLegacyRootGeneration == 0) {
      return false;
    }
    if (get32(input) != Magic || get16(input + 4) != Version ||
        get32(input + CrcOffset) != crc32(input, CrcOffset)) {
      return false;
    }

    const uint16_t count = get16(input + 6);
    const uint32_t generation = get32(input + 8);
    const uint32_t rootGeneration = get32(input + 52);
    const uint32_t rootCrc = get32(input + 56);
    if (count > Capacity || generation == 0 || rootGeneration == 0 ||
        rootGeneration != expectedLegacyRootGeneration ||
        rootCrc != expectedLegacyRootCrc ||
        get32(input + 12) != expectedLocal.node ||
        get32(input + 48) != expectedLocal.generation ||
        memcmp(input + 16, expectedLocal.publicKey, 32) != 0) {
      return false;
    }

    Value candidate{};
    candidate.generation = generation;
    candidate.localIdentityGeneration = get32(input + 48);
    candidate.legacyRootGeneration = rootGeneration;
    candidate.legacyRootCrc = rootCrc;
    candidate.count = count;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t *row = input + HeaderBytes + i * RowBytes;
      candidate.contacts[i].node = get32(row);
      memcpy(candidate.contacts[i].publicKey, row + 4, 32);
      if (i && candidate.contacts[i - 1].node >= candidate.contacts[i].node) {
        return false;
      }
    }
    const size_t unusedOffset = HeaderBytes + size_t(count) * RowBytes;
    if (!allZero(input + unusedOffset, CrcOffset - unusedOffset)) return false;

    PkiPeerSet validated;
    if (!validated.assignEmpty(expectedLocal)) return false;
    if (count && !validated.assignTrusted(expectedLocal, candidate.contacts, count)) {
      return false;
    }
    out = candidate;
    return true;
  }

  static uint32_t crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
      crc ^= bytes[i];
      for (unsigned bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
      }
    }
    return ~crc;
  }

private:
  static uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
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
  static bool allZero(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; ++i) if (p[i] != 0) return false;
    return true;
  }
};

} // namespace mt::pki
