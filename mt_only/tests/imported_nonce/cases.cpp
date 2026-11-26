// SPDX-License-Identifier: GPL-3.0-only
#include "../../src/pki/ImportedNoncePermutation.h"
#include <cassert>
#include <cstdio>

namespace bench {
// The linked test copy of MtCodec.cpp exports the native backend under this name.
bool systemAesCtr(void *, const uint8_t *, size_t, const uint8_t[16], uint8_t *, size_t);
static unsigned calls = 0, failAt = 0;
bool mtAesCtr(void *ctx, const uint8_t *key, size_t keySize, const uint8_t iv[16],
              uint8_t *bytes, size_t count) {
  if (++calls == failAt) return false;
  return systemAesCtr(ctx, key, keySize, iv, bytes, count);
}
}

int main() {
  using mt::pki::ImportedNoncePermutation;
  // Bouncy Castle 1.83 FPEFF1Engine with an empty tweak and radix 256.
  // Column 1 key bytes are 0..31. Column 2 uses 255..224.
  const uint32_t known[][3] = {
    {0x00000000, 0xbaace458, 0x87621cf2},
    {0x00000001, 0x58b209bc, 0x8bf1dedc},
    {0x000000ff, 0x5d0733e0, 0xbbedd9ac},
    {0x00000100, 0x440e7713, 0x517e68b9},
    {0x0000ffff, 0x3dd75098, 0xc4121b86},
    {0x00010000, 0x5a9a4d6b, 0x1fb6b04b},
    {0x1234abcd, 0xcca9e45b, 0x36fd321a},
    {0x80000000, 0x202aafe4, 0xc02f28b6},
    {0xfffffffe, 0x3a6f2d37, 0x5f7aa350},
    {0xffffffff, 0xfffdd4d5, 0xf0fcca9c}
  };
  const uint32_t counterForZero[] = {0xc714583c, 0x87b764ee};
  for (unsigned column = 0; column < 2; ++column) {
    uint8_t key[32];
    for (unsigned i = 0; i < sizeof(key); ++i) key[i] = column == 0 ? i : 255 - i;
    uint32_t extra = 123;
    for (const auto &row : known) {
      assert(ImportedNoncePermutation::apply(key, row[0], extra));
      assert(extra == row[column + 1]);
    }
    assert(ImportedNoncePermutation::apply(key, counterForZero[column], extra) && extra == 0);
  }
  uint32_t unchanged = 123;
  assert(!ImportedNoncePermutation::apply(nullptr, 456, unchanged) && unchanged == 123);
  uint8_t testKey[32]{};
  for (unsigned failAt = 1; failAt <= 11; ++failAt) {
    bench::calls = 0;
    bench::failAt = failAt;
    assert(!ImportedNoncePermutation::apply(testKey, 456, unchanged));
    assert(unchanged == 123 && bench::calls == failAt);
  }
  puts("22 independent FF1 vectors and all 11 AES failure positions passed");
}
