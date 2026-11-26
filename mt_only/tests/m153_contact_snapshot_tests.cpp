// SPDX-License-Identifier: GPL-3.0-only
// Host-only pure-codec tests with synthetic public identities; no storage/I/O.
#include "pki/PkiContactSnapshot.h"
#include "monocypher.h"

#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

using namespace mt::pki;

namespace {
struct Key {
  uint8_t scalar[32]{};
  IdentityBinding binding{};
  explicit Key(uint8_t seed) {
    for (unsigned i = 0; i < 32; ++i) scalar[i] = uint8_t(seed + 31u * i);
    scalar[0] &= 248;
    scalar[31] = uint8_t((scalar[31] & 127) | 64);
    crypto_x25519_public_key(binding.publicKey, scalar);
    binding.node = bench::MtSignature::nodeId(binding.publicKey);
    binding.generation = 1;
  }
  ~Key() { crypto_wipe(scalar, sizeof(scalar)); }
};

void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
}
uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void reseal(uint8_t bytes[PkiContactSnapshot::RecordBytes]) {
  put32(bytes + PkiContactSnapshot::CrcOffset,
        PkiContactSnapshot::crc32(bytes, PkiContactSnapshot::CrcOffset));
}
bool same(const PkiContactSnapshot::Value &a,
          const PkiContactSnapshot::Value &b) {
  if (a.generation != b.generation ||
      a.localIdentityGeneration != b.localIdentityGeneration ||
      a.legacyRootGeneration != b.legacyRootGeneration ||
      a.legacyRootCrc != b.legacyRootCrc || a.count != b.count) return false;
  for (size_t i = 0; i < a.count; ++i) {
    if (a.contacts[i].node != b.contacts[i].node ||
        memcmp(a.contacts[i].publicKey, b.contacts[i].publicKey, 32)) return false;
  }
  return true;
}
void copyPin(PkiPeerPin &out, const IdentityBinding &in) {
  out.node = in.node;
  memcpy(out.publicKey, in.publicKey, 32);
}

void roundTripAndCapacity() {
  Key local(7);
  Key keys[17] = {Key(20),Key(21),Key(22),Key(23),Key(24),Key(25),Key(26),Key(27),
                  Key(28),Key(29),Key(30),Key(31),Key(32),Key(33),Key(34),Key(35),Key(36)};
  PkiPeerPin peers[16]{};
  for (size_t i = 0; i < 16; ++i) copyPin(peers[i], keys[i].binding);
  uint8_t encoded[PkiContactSnapshot::RecordBytes];
  memset(encoded, 0xa5, sizeof(encoded));
  assert(PkiContactSnapshot::encode(local.binding, UINT32_MAX, 9, 0x87654321u,
                                    peers, 16, encoded, sizeof(encoded)));
  assert(get32(encoded + 8) == UINT32_MAX);
  assert(get32(encoded + 48) == local.binding.generation);
  assert(get32(encoded + PkiContactSnapshot::CrcOffset) ==
         PkiContactSnapshot::crc32(encoded, PkiContactSnapshot::CrcOffset));
  PkiContactSnapshot::Value decoded{};
  assert(PkiContactSnapshot::decode(encoded, sizeof(encoded), local.binding,
                                    9, 0x87654321u, decoded));
  assert(decoded.generation == UINT32_MAX && decoded.count == 16);
  assert(decoded.localIdentityGeneration == local.binding.generation);
  for (size_t i = 0; i < 16; ++i) {
    if (i) assert(decoded.contacts[i - 1].node < decoded.contacts[i].node);
    bool found = false;
    for (const auto &peer : peers) {
      if (decoded.contacts[i].node == peer.node &&
          !memcmp(decoded.contacts[i].publicKey, peer.publicKey, 32)) found = true;
    }
    assert(found);
  }
  uint8_t tooMany[PkiContactSnapshot::RecordBytes];
  memset(tooMany, 0x5a, sizeof(tooMany));
  assert(!PkiContactSnapshot::encode(local.binding, 1, 1, 0, peers, 17,
                                     tooMany, sizeof(tooMany)));
  for (uint8_t b : tooMany) assert(b == 0x5a);
}

void emptyAndCanonicalUnusedRows() {
  Key local(41);
  uint8_t bytes[PkiContactSnapshot::RecordBytes];
  assert(PkiContactSnapshot::encode(local.binding, 1, 3, 0x10203040u,
                                    nullptr, 0, bytes, sizeof(bytes)));
  for (size_t i = PkiContactSnapshot::HeaderBytes; i < PkiContactSnapshot::CrcOffset; ++i)
    assert(bytes[i] == 0);
  PkiContactSnapshot::Value value{};
  assert(PkiContactSnapshot::decode(bytes, sizeof(bytes), local.binding,
                                    3, 0x10203040u, value));
  assert(value.count == 0 && value.generation == 1);

  Key peer(42);
  PkiPeerPin pin{}; copyPin(pin, peer.binding);
  assert(PkiContactSnapshot::encode(local.binding, 2, 3, 0x10203040u,
                                    &pin, 1, bytes, sizeof(bytes)));
  const size_t unused = PkiContactSnapshot::HeaderBytes + PkiContactSnapshot::RowBytes;
  assert(bytes[unused] == 0 && bytes[PkiContactSnapshot::CrcOffset - 1] == 0);
  bytes[unused + 3] = 1;
  reseal(bytes);
  value.generation = 909; value.count = 2;
  const auto sentinel = value;
  assert(!PkiContactSnapshot::decode(bytes, sizeof(bytes), local.binding,
                                     3, 0x10203040u, value));
  assert(same(value, sentinel));
}

void bindingLengthAndCorruptionReject() {
  Key local(50), peer(51), otherLocal(52);
  PkiPeerPin pin{}; copyPin(pin, peer.binding);
  uint8_t good[PkiContactSnapshot::RecordBytes];
  assert(PkiContactSnapshot::encode(local.binding, 4, 12, 0xaabbccdd,
                                    &pin, 1, good, sizeof(good)));
  PkiContactSnapshot::Value out{};
  out.generation = 77; out.count = 3;
  const auto unchanged = out;
  assert(!PkiContactSnapshot::decode(good, sizeof(good) - 1, local.binding,
                                     12, 0xaabbccdd, out));
  assert(same(out, unchanged));
  assert(!PkiContactSnapshot::decode(good, sizeof(good), otherLocal.binding,
                                     12, 0xaabbccdd, out));
  IdentityBinding changedGeneration = local.binding;
  ++changedGeneration.generation;
  assert(!PkiContactSnapshot::decode(good, sizeof(good), changedGeneration,
                                     12, 0xaabbccdd, out));
  assert(same(out, unchanged));
  uint8_t wrongIdentityGeneration[PkiContactSnapshot::RecordBytes];
  memcpy(wrongIdentityGeneration, good, sizeof(good));
  put32(wrongIdentityGeneration + 48, local.binding.generation + 1);
  reseal(wrongIdentityGeneration);
  assert(!PkiContactSnapshot::decode(wrongIdentityGeneration, sizeof(good), local.binding,
                                     12, 0xaabbccdd, out));
  assert(same(out, unchanged));
  assert(!PkiContactSnapshot::decode(good, sizeof(good), local.binding,
                                     13, 0xaabbccdd, out));
  assert(!PkiContactSnapshot::decode(good, sizeof(good), local.binding,
                                     12, 0xaabbccde, out));
  uint8_t badCount[PkiContactSnapshot::RecordBytes];
  memcpy(badCount, good, sizeof(good)); badCount[6] = 17; badCount[7] = 0; reseal(badCount);
  assert(!PkiContactSnapshot::decode(badCount, sizeof(badCount), local.binding,
                                     12, 0xaabbccdd, out));
  uint8_t badVersion[PkiContactSnapshot::RecordBytes];
  memcpy(badVersion, good, sizeof(good)); badVersion[4] = 2; reseal(badVersion);
  assert(!PkiContactSnapshot::decode(badVersion, sizeof(badVersion), local.binding,
                                     12, 0xaabbccdd, out));
  for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(8),
                        size_t(12), size_t(PkiContactSnapshot::CrcOffset)}) {
    uint8_t damaged[PkiContactSnapshot::RecordBytes];
    memcpy(damaged, good, sizeof(good)); damaged[offset] ^= 0x01;
    assert(!PkiContactSnapshot::decode(damaged, sizeof(damaged), local.binding,
                                       12, 0xaabbccdd, out));
  }
  assert(!PkiContactSnapshot::encode(local.binding, 0, 12, 0, &pin, 1, good, sizeof(good)));
  assert(!PkiContactSnapshot::encode(local.binding, 1, 0, 0, &pin, 1, good, sizeof(good)));
  assert(!PkiContactSnapshot::encode(local.binding, 1, 12, 0, &pin, 1, good, sizeof(good)-1));
  IdentityBinding invalid = local.binding; invalid.generation = 0;
  assert(!PkiContactSnapshot::encode(invalid, 1, 12, 0, &pin, 1, good, sizeof(good)));
  invalid = local.binding; invalid.node ^= 1;
  assert(!PkiContactSnapshot::encode(invalid, 1, 12, 0, &pin, 1, good, sizeof(good)));
}

void duplicateAndInvalidRowsReject() {
  Key local(61), a(62), b(63);
  PkiPeerPin rows[2]{};
  copyPin(rows[0], a.binding); copyPin(rows[1], a.binding);
  uint8_t bytes[PkiContactSnapshot::RecordBytes];
  uint8_t original[PkiContactSnapshot::RecordBytes];
  memset(bytes, 0xA5, sizeof(bytes)); memcpy(original, bytes, sizeof(bytes));
  assert(!PkiContactSnapshot::encode(local.binding, 1, 2, 3, rows, 2, bytes, sizeof(bytes)));
  assert(!memcmp(bytes, original, sizeof(bytes)));
  copyPin(rows[1], b.binding);
  rows[1].node = rows[0].node; // duplicate node with a different public key
  assert(!PkiContactSnapshot::encode(local.binding, 1, 2, 3, rows, 2, bytes, sizeof(bytes)));
  assert(!memcmp(bytes, original, sizeof(bytes)));
  copyPin(rows[1], b.binding);
  memcpy(rows[1].publicKey, rows[0].publicKey, 32); // duplicate public key
  assert(!PkiContactSnapshot::encode(local.binding, 1, 2, 3, rows, 2, bytes, sizeof(bytes)));
  assert(!memcmp(bytes, original, sizeof(bytes)));
  copyPin(rows[0], a.binding); rows[0].node = 1;
  assert(!PkiContactSnapshot::encode(local.binding, 1, 2, 3, rows, 1, bytes, sizeof(bytes)));

  copyPin(rows[0], a.binding); copyPin(rows[1], b.binding);
  assert(PkiContactSnapshot::encode(local.binding, 1, 2, 3, rows, 2, bytes, sizeof(bytes)));
  PkiContactSnapshot::Value out{};
  out.generation = 515; out.count = 4;
  const auto sentinel = out;
  // Keep CRC valid so semantic duplicate detection is exercised.
  memcpy(bytes + PkiContactSnapshot::HeaderBytes + PkiContactSnapshot::RowBytes + 4,
         bytes + PkiContactSnapshot::HeaderBytes + 4, 32);
  reseal(bytes);
  assert(!PkiContactSnapshot::decode(bytes, sizeof(bytes), local.binding, 2, 3, out));
  assert(same(out, sentinel));

  assert(PkiContactSnapshot::encode(local.binding, 1, 2, 3, rows, 2, bytes, sizeof(bytes)));
  uint8_t row[PkiContactSnapshot::RowBytes];
  memcpy(row, bytes + PkiContactSnapshot::HeaderBytes, sizeof(row));
  memcpy(bytes + PkiContactSnapshot::HeaderBytes,
         bytes + PkiContactSnapshot::HeaderBytes + PkiContactSnapshot::RowBytes, sizeof(row));
  memcpy(bytes + PkiContactSnapshot::HeaderBytes + PkiContactSnapshot::RowBytes,
         row, sizeof(row));
  reseal(bytes);
  assert(!PkiContactSnapshot::decode(bytes, sizeof(bytes), local.binding, 2, 3, out));
  assert(same(out, sentinel));
}
} // namespace

int main() {
  roundTripAndCapacity();
  emptyAndCanonicalUnusedRows();
  bindingLengthAndCorruptionReject();
  duplicateAndInvalidRowsReject();
  puts("PASS M153 fixed contact snapshot canonical codec: 16 capacity, identity/root binding, CRC, canonical padding, and malformed/duplicate rejection");
  return 0;
}
