// SPDX-License-Identifier: GPL-3.0-only
#include "pki/PkiNonceCounter.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

using mt::pki::PkiNonceCounter;
using mt::pki::PkiNonceCounterBinding;
using mt::pki::PkiNonceCounterStore;

namespace {
uint32_t crc32(const uint8_t *bytes, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
  }
  return ~crc;
}

void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
}

PkiNonceCounterBinding binding() {
  PkiNonceCounterBinding b{};
  b.node = 0x12345678u;
  for (unsigned i = 0; i < sizeof(b.publicKey); ++i)
    b.publicKey[i] = uint8_t(i + 1u);
  b.legacyGeneration = 2653;
  b.legacyCrc = 0x8a7b6c5du;
  return b;
}

void makeRecord(uint8_t out[PkiNonceCounter::RecordBytes],
                const PkiNonceCounterBinding &b, uint32_t through,
                uint32_t generation) {
  memset(out, 0, PkiNonceCounter::RecordBytes);
  out[0] = 'Z'; out[1] = 'T'; out[2] = 'N'; out[3] = 'C'; out[4] = 1;
  put32(out + 8, b.node);
  memcpy(out + 12, b.publicKey, 32);
  put32(out + 44, b.legacyGeneration);
  put32(out + 48, b.legacyCrc);
  put32(out + 52, through);
  put32(out + 56, generation);
  put32(out + 60, crc32(out, 60));
}

struct MemoryStore final : PkiNonceCounterStore {
  enum class WriteMode { Normal, FailBefore, CommitThenFail, CorruptCommit };
  uint8_t slots[2][PkiNonceCounter::RecordBytes]{};
  bool present[2]{};
  int reads = 0, writes = 0;
  int failReadSlot = -1;
  int successfulWritesBeforeFailure = 0;
  WriteMode mode = WriteMode::Normal;

  int read(unsigned slot, uint8_t *out, size_t capacity) override {
    ++reads;
    if (slot > 1 || !out || capacity != PkiNonceCounter::RecordBytes)
      return -EINVAL;
    if (int(slot) == failReadSlot) return -EIO;
    if (!present[slot]) return -ENOENT;
    memcpy(out, slots[slot], capacity);
    return int(capacity);
  }
  int write(unsigned slot, const uint8_t *data, size_t size) override {
    ++writes;
    if (slot > 1 || !data || size != PkiNonceCounter::RecordBytes)
      return -EINVAL;
    if (mode == WriteMode::FailBefore) {
      if (!successfulWritesBeforeFailure) return -EIO;
      --successfulWritesBeforeFailure;
    }
    memcpy(slots[slot], data, size);
    present[slot] = true;
    if (mode == WriteMode::CorruptCommit) slots[slot][60] ^= 0x80;
    if (mode == WriteMode::CommitThenFail) return -EIO;
    return 0;
  }
};

void testGenesisAndBurnOnReopen() {
  MemoryStore store;
  PkiNonceCounter counter(store);
  const auto b = binding();
  assert(counter.initialize(b, true) == 0);
  assert(store.writes == 2 && store.present[0] && store.present[1]);
  assert(memcmp(store.slots[0], store.slots[1], PkiNonceCounter::RecordBytes) == 0);
  assert(!counter.active() && counter.reservedThrough() == 0);
  uint32_t value = 99;
  assert(counter.reserve(value) == -EACCES && value == 0);
  assert(counter.open(b, false) == -EACCES);
  assert(counter.open(b, true) == 0);
  assert(counter.active() && counter.reservedThrough() == 0);
  for (uint32_t expected = 1; expected <= 256; ++expected) {
    assert(counter.reserve(value) == 0 && value == expected);
  }
  assert(counter.active() && counter.reservedThrough() == 256);
  counter.close();
  assert(!counter.active() && counter.reservedThrough() == 0);
  assert(counter.open(b, true) == 0);
  assert(counter.reserve(value) == 0 && value == 257);
  counter.close();
  assert(counter.open(b, true) == 0);
  assert(counter.reserve(value) == 0 && value == 513);
}

void testPartialGenesisAndAuthority() {
  MemoryStore store;
  PkiNonceCounter counter(store);
  const auto b = binding();
  assert(counter.initialize(b, false) == -EACCES && store.writes == 0);
  store.mode = MemoryStore::WriteMode::FailBefore;
  store.successfulWritesBeforeFailure = 1;
  // Slot zero commits genesis; slot one's write fails.
  assert(counter.initialize(b, true) == -EIO);
  assert(store.present[0] && !store.present[1]);
  uint32_t blocked = 1;
  assert(counter.reserve(blocked) == -EACCES && blocked == 0);
  assert(counter.initialize(b, true) == -EACCES);
  store.mode = MemoryStore::WriteMode::Normal;
  const int before = store.writes;
  PkiNonceCounter resumed(store); // Fresh object; caller must re-prove authority.
  assert(resumed.initialize(b, true) == 0);
  assert(store.writes == before + 1 && store.present[1]);
  assert(resumed.open(b, true) == 0);
  uint32_t value = 0;
  assert(resumed.reserve(value) == 0 && value == 1);

  MemoryStore uncertain;
  uncertain.mode = MemoryStore::WriteMode::CommitThenFail;
  PkiNonceCounter first(uncertain);
  assert(first.initialize(b, true) == -EIO);
  assert(uncertain.present[0] && !uncertain.present[1]);
  assert(!first.active());
  uncertain.mode = MemoryStore::WriteMode::Normal;
  PkiNonceCounter afterUncertain(uncertain);
  assert(afterUncertain.initialize(b, true) == 0);
  assert(afterUncertain.open(b, true) == 0);
  assert(afterUncertain.reserve(value) == 0 && value == 1);

  MemoryStore corruptBootstrap;
  corruptBootstrap.mode = MemoryStore::WriteMode::CorruptCommit;
  PkiNonceCounter corrupted(corruptBootstrap);
  assert(corrupted.initialize(b, true) == -EIO);
  assert(corrupted.initialize(b, true) == -EACCES);
  assert(!corrupted.active());
}

void testWriteUncertaintyBurnsOrDoesNotIssue() {
  const auto b = binding();
  {
    MemoryStore store;
    PkiNonceCounter counter(store);
    assert(counter.initialize(b, true) == 0);
    assert(counter.open(b, true) == 0);
    store.mode = MemoryStore::WriteMode::FailBefore;
    uint32_t value = 77;
    assert(counter.reserve(value) == -EIO && value == 0);
    store.mode = MemoryStore::WriteMode::Normal;
    assert(counter.reserve(value) == -EACCES && value == 0);
    assert(counter.open(b, true) == 0);
    assert(counter.reserve(value) == 0 && value == 1);
  }
  {
    MemoryStore store;
    PkiNonceCounter counter(store);
    assert(counter.initialize(b, true) == 0);
    assert(counter.open(b, true) == 0);
    store.mode = MemoryStore::WriteMode::CommitThenFail;
    uint32_t value = 77;
    assert(counter.reserve(value) == -EIO && value == 0);
    store.mode = MemoryStore::WriteMode::Normal;
    assert(counter.open(b, true) == 0);
    assert(counter.reserve(value) == 0 && value == 257);
  }
}

void testReadbackFailureAndCorruption() {
  const auto b = binding();
  {
    MemoryStore store;
    PkiNonceCounter counter(store);
    assert(counter.initialize(b, true) == 0);
    assert(counter.open(b, true) == 0);
    store.mode = MemoryStore::WriteMode::CorruptCommit;
    uint32_t value = 12;
    assert(counter.reserve(value) == -EIO && value == 0);
    store.mode = MemoryStore::WriteMode::Normal;
    assert(counter.open(b, true) == -ENODATA);
    assert(counter.reserve(value) == -EACCES && value == 0);
  }
  {
    MemoryStore store;
    PkiNonceCounter counter(store);
    assert(counter.initialize(b, true) == 0);
    store.slots[1][20] ^= 1;
    const int rc = counter.open(b, true);
    assert(rc == -ENODATA || rc == -EILSEQ);
  }
}

void testMissingMismatchAndInvalidBinding() {
  const auto b = binding();
  {
    MemoryStore store;
    PkiNonceCounter counter(store);
    assert(counter.initialize(b, true) == 0);
    store.present[1] = false;
    assert(counter.open(b, true) == -ENODATA);
  }
  {
    MemoryStore store;
    PkiNonceCounter counter(store);
    assert(counter.initialize(b, true) == 0);
    auto other = b;
    other.publicKey[0] ^= 1;
    assert(counter.open(other, true) == -EXDEV);
    other = b; other.node++;
    assert(counter.open(other, true) == -EXDEV);
    other = b; other.legacyGeneration++;
    assert(counter.open(other, true) == -EXDEV);
    other = b; other.legacyCrc++;
    assert(counter.open(other, true) == -EXDEV);
    other = b; other.publicKey[0] = 0;
    for (unsigned i = 1; i < sizeof(other.publicKey); ++i) other.publicKey[i] = 0;
    assert(counter.open(other, true) == -EACCES);
  }
}

void testInitializationNeverRepairsNonGenesis() {
  MemoryStore store;
  const auto b = binding();
  makeRecord(store.slots[0], b, 256, 2);
  makeRecord(store.slots[1], b, 256, 2);
  store.present[0] = store.present[1] = true;
  PkiNonceCounter counter(store);
  assert(counter.initialize(b, true) == -EACCES);
  assert(store.writes == 0);
}

void testConflictingRecordsRejected() {
  const auto b = binding();
  {
    MemoryStore store;
    makeRecord(store.slots[0], b, 0, 1);
    makeRecord(store.slots[1], b, 512, 3); // Skipped generation.
    store.present[0] = store.present[1] = true;
    PkiNonceCounter counter(store);
    assert(counter.open(b, true) == -EILSEQ);
  }
  {
    MemoryStore store;
    makeRecord(store.slots[0], b, 0, 1);
    makeRecord(store.slots[1], b, 256, 2);
    store.slots[1][52] = 1; // Inconsistent counter with a corrected CRC.
    put32(store.slots[1] + 60, crc32(store.slots[1], 60));
    store.present[0] = store.present[1] = true;
    PkiNonceCounter counter(store);
    assert(counter.open(b, true) == -ENODATA);
  }
  {
    MemoryStore store;
    makeRecord(store.slots[0], b, 0, 1);
    makeRecord(store.slots[1], b, 0, 1);
    store.slots[1][44] ^= 1;
    put32(store.slots[1] + 60, crc32(store.slots[1], 60));
    store.present[0] = store.present[1] = true;
    PkiNonceCounter counter(store);
    assert(counter.open(b, true) == -EXDEV);
  }
}

void testGenerationOverflowAndCounterExhaustion() {
  const auto b = binding();
  MemoryStore store;
  const uint32_t finalGeneration =
      uint32_t((uint64_t(UINT32_MAX) + PkiNonceCounter::DefaultBlockSize - 1u) /
                   PkiNonceCounter::DefaultBlockSize + 1u);
  makeRecord(store.slots[0], b, UINT32_MAX, finalGeneration);
  memcpy(store.slots[1], store.slots[0], PkiNonceCounter::RecordBytes);
  store.present[0] = store.present[1] = true;
  PkiNonceCounter counter(store);
  assert(counter.open(b, true) == 0);
  uint32_t value = 77;
  assert(counter.reserve(value) == -ENOSPC && value == 0);
  assert(counter.reserve(value) == -EACCES && value == 0);
  assert(store.writes == 0);
}

void testStoreReadErrorsFailClosed() {
  MemoryStore store;
  PkiNonceCounter counter(store);
  const auto b = binding();
  assert(counter.initialize(b, true) == 0);
  store.failReadSlot = 1;
  assert(counter.open(b, true) == -EIO);
  uint32_t value = 42;
  assert(counter.reserve(value) == -EACCES && value == 0);
}
} // namespace

int main() {
  testGenesisAndBurnOnReopen();
  testPartialGenesisAndAuthority();
  testWriteUncertaintyBurnsOrDoesNotIssue();
  testReadbackFailureAndCorruption();
  testMissingMismatchAndInvalidBinding();
  testInitializationNeverRepairsNonGenesis();
  testConflictingRecordsRejected();
  testGenerationOverflowAndCounterExhaustion();
  testStoreReadErrorsFailClosed();
}
