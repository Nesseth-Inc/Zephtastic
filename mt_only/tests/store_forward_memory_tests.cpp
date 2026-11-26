// SPDX-License-Identifier: GPL-3.0-only
#include "modules/StoreForwardMemory.h"

#include <assert.h>
#include <stdint.h>

using mt::modules::StoreForwardArchive;
using mt::modules::StoreForwardMemory;

#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
static StoreForwardArchive::MessageView message(uint32_t time, uint32_t id) {
  static const uint8_t payload[] = {0x41, 0x42, 0x43};
  StoreForwardArchive::MessageView out{};
  out.time = time;
  out.from = 100;
  out.to = StoreForwardArchive::BroadcastNode;
  out.id = id;
  out.payload = payload;
  out.payloadSize = sizeof(payload);
  return out;
}

static void testEnabledArena() {
  StoreForwardMemory memory;
  assert(memory.initialize(true, true, 4));
  auto s = memory.snapshot();
  assert(s.available && s.capacity == 4 && s.usedRecords == 0 && s.freeRecords == 4);
  assert(s.reservedBytes >= 4 * sizeof(StoreForwardArchive::Record));
  assert(s.reservedBytes <= StoreForwardMemory::MaxReservedBytes);
  assert(s.freeBytes == 4 * sizeof(StoreForwardArchive::Record));

  StoreForwardArchive *archive = memory.archive();
  assert(archive && archive->ready());
  for (uint32_t i = 1; i <= 4; ++i)
    assert(archive->add(message(i, i)) == StoreForwardArchive::AddResult::Added);
  size_t count = 0;
  assert(archive->countAvailable(77, 0, count) && count == 4);
  size_t next = 0;
  assert(archive->cursor(77, next) && next == 0);
  s = memory.snapshot();
  assert(s.available && s.usedRecords == 4 && s.freeRecords == 0 && s.freeBytes == 0);

  memory.disable();
  s = memory.snapshot();
  assert(!s.available && s.capacity == 4 && s.usedRecords == 0 && s.freeRecords == 4);
  assert(s.reservedBytes != 0 && s.freeBytes == 4 * sizeof(StoreForwardArchive::Record));
  assert(memory.archive() == nullptr);

  assert(memory.initialize(true, true, 4));
  archive = memory.archive();
  assert(archive && archive->size() == 0);
  assert(!archive->cursor(77, next));
  assert(archive->add(message(10, 10)) == StoreForwardArchive::AddResult::Added);
  memory.disable();
}

static void testReadinessAndOwnerExclusion() {
  StoreForwardMemory first;
  StoreForwardMemory second;
  assert(first.initialize(true, true, 3));
  StoreForwardArchive *prior = first.archive();
  assert(prior && prior->add(message(5, 5)) == StoreForwardArchive::AddResult::Added);
  size_t count = 0, next = 0;
  assert(prior->countAvailable(91, 0, count) && count == 1);
  assert(prior->cursor(91, next));

  assert(!second.initialize(true, true, 3));
  assert(second.lastError() == StoreForwardMemory::Error::Busy);
  assert(first.snapshot().available && prior->size() == 1);

  // Keep the archive object alive but inaccessible through the adapter. Its
  // data and cursor remain readable here, proving false readiness did not
  // clear or canary-write the shared backing arrays.
  assert(!first.initialize(false, true, 3));
  assert(first.lastError() == StoreForwardMemory::Error::CallerNotReady);
  assert(!first.snapshot().available && first.archive() == nullptr);
  assert(prior->size() == 1 && prior->cursor(91, next));

  // A fresh readiness admission performs the bounded canary and resets the
  // volatile archive before it becomes available again.
  assert(first.initialize(true, true, 3));
  assert(first.archive() && first.archive()->size() == 0);
  assert(!first.archive()->cursor(91, next));
  first.disable();
}

static void testCapacityRejection() {
  StoreForwardMemory memory;
  assert(!memory.initialize(true, true, 0));
  assert(memory.lastError() == StoreForwardMemory::Error::InvalidCapacity);
  assert(!memory.initialize(true, true, StoreForwardMemory::MaxRecords + 1));
  assert(memory.lastError() == StoreForwardMemory::Error::CapacityExceeded);
  assert(!memory.snapshot().available);
}

static void testExclusiveOwnerReleasedByDestruction() {
  StoreForwardMemory second;
  {
    StoreForwardMemory first;
    assert(first.initialize(true, true, 2));
    assert(!second.initialize(true, true, 2));
    assert(second.lastError() == StoreForwardMemory::Error::Busy);
  }
  assert(second.initialize(true, true, 2));
  assert(second.archive() && second.archive()->size() == 0);
  second.disable();
}
#endif

int main() {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  testEnabledArena();
  testReadinessAndOwnerExclusion();
  testCapacityRejection();
  testExclusiveOwnerReleasedByDestruction();
#else
  StoreForwardMemory memory;
  assert(!memory.initialize(true, true, 4));
  assert(memory.lastError() == StoreForwardMemory::Error::FeatureDisabled);
  const auto s = memory.snapshot();
  assert(!s.available && s.reservedBytes == 0);
#endif
}
