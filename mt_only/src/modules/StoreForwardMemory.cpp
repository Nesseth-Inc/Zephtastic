// SPDX-License-Identifier: GPL-3.0-only
#include "StoreForwardMemory.h"
#if defined(__ZEPHYR__) && defined(MT_STORE_FORWARD_MEMORY_NATIVE_TEST)
#error "Native section bypass is forbidden in embedded builds"
#endif

#include <new>
#include <string.h>

namespace mt::modules {
namespace {

#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
constexpr uint32_t GuardBefore = 0x4D543135u;
constexpr uint32_t GuardAfter = 0xA8C7E219u;
constexpr uint8_t CanarySeed = 0x6Du;
StoreForwardMemory *arenaOwner = nullptr;

struct alignas(16) StoreForwardArena {
  uint32_t guardBefore;
  alignas(16) StoreForwardArchive::Record records[StoreForwardMemory::MaxRecords];
  alignas(16) StoreForwardArchive::Cursor cursors[StoreForwardMemory::MaxCursors];
  uint32_t guardAfter;
};

// This is the only object placed in the archive's external-BSS section. It has
// no constructor; the Archive object itself is constructed later in caller RAM.
#if defined(MT_STORE_FORWARD_MEMORY_NATIVE_TEST)
// Mach-O does not accept ELF's section spelling. Native tests exercise the
// exact arena bytes/layout without pretending to verify embedded placement.
StoreForwardArena arena __attribute__((aligned(16)));
#else
StoreForwardArena arena __attribute__((section(".ext_ram.bss.mt_store_forward"), aligned(16)));
#endif

constexpr size_t RecordBytes = sizeof(arena.records);
constexpr size_t CursorBytes = sizeof(arena.cursors);
constexpr size_t BackingBytes = RecordBytes + CursorBytes;

static_assert(alignof(StoreForwardArena) >= 16, "PSRAM arena alignment");
static_assert(sizeof(arena.records) == StoreForwardMemory::MaxRecords *
                                          sizeof(StoreForwardArchive::Record),
              "record array must be contiguous");
static_assert(sizeof(arena.cursors) == StoreForwardMemory::MaxCursors *
                                           sizeof(StoreForwardArchive::Cursor),
              "cursor array must be contiguous");
static_assert(sizeof(StoreForwardArena) <= StoreForwardMemory::MaxReservedBytes,
              "archive aggregate exceeds 128 KiB budget");
static_assert(BackingBytes <= StoreForwardMemory::MaxReservedBytes,
              "archive arrays exceed 128 KiB budget");

bool runArenaCanary() {
  arena.guardBefore = GuardBefore;
  arena.guardAfter = GuardAfter;

  // Exercise every byte in the two owned arrays, with volatile accesses so
  // the check remains a bounded PSRAM read/write test after optimization.
  volatile uint8_t *records = reinterpret_cast<volatile uint8_t *>(arena.records);
  volatile uint8_t *cursors = reinterpret_cast<volatile uint8_t *>(arena.cursors);
  for (size_t i = 0; i < RecordBytes; ++i)
    records[i] = uint8_t(CanarySeed ^ uint8_t(i * 29u));
  for (size_t i = 0; i < CursorBytes; ++i)
    cursors[i] = uint8_t(CanarySeed ^ uint8_t(i * 47u));

  bool ok = true;
  for (size_t i = 0; i < RecordBytes; ++i) {
    const bool matches = records[i] == uint8_t(CanarySeed ^ uint8_t(i * 29u));
    ok = matches && ok;
  }
  for (size_t i = 0; i < CursorBytes; ++i) {
    const bool matches = cursors[i] == uint8_t(CanarySeed ^ uint8_t(i * 47u));
    ok = matches && ok;
  }
  return ok && arena.guardBefore == GuardBefore && arena.guardAfter == GuardAfter;
}
#endif

}  // namespace

StoreForwardMemory::~StoreForwardMemory() {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  if (arenaOwner == this) arenaOwner = nullptr;
#endif
  if (archive_) archive_->~StoreForwardArchive();
}

bool StoreForwardMemory::initialize(bool callerReady, bool enabled,
                                    size_t configuredRecords) {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  // Do this first: readiness failure must not clear, canary, or otherwise
  // touch PSRAM, even when this instance owned a previously initialized arena.
  if (!callerReady) {
    enabled_ = false;
    configuredRecords_ = 0;
    error_ = Error::CallerNotReady;
    if (arenaOwner == this) arenaOwner = nullptr;
    return false;
  }
  if (configuredRecords == 0) {
    error_ = Error::InvalidCapacity;
    return false;
  }
  if (configuredRecords > MaxRecords) {
    error_ = Error::CapacityExceeded;
    return false;
  }
  if (!enabled) {
    if (arenaOwner == this) {
      if (archive_) archive_->clear();
      if (archive_) archive_->~StoreForwardArchive();
      archive_ = nullptr;
      arenaOwner = nullptr;
    }
    enabled_ = false;
    configuredRecords_ = configuredRecords;
    error_ = Error::Disabled;
    return false;
  }
  if (arenaOwner && arenaOwner != this) {
    error_ = Error::Busy;
    return false;
  }

  // Claim before any canary or archive operation. This is a serialized
  // single-owner contract, not a lock; the caller owns synchronization.
  arenaOwner = this;
  enabled_ = false;
  configuredRecords_ = configuredRecords;
  if (archive_) {
    archive_->~StoreForwardArchive();
    archive_ = nullptr;
  }
  if (!runArenaCanary()) {
    arenaOwner = nullptr;
    error_ = Error::CanaryFailed;
    return false;
  }

  memset(arena.records, 0, sizeof(arena.records));
  memset(arena.cursors, 0, sizeof(arena.cursors));
  archive_ = new (archiveStorage_) StoreForwardArchive(
      arena.records, configuredRecords, arena.cursors, MaxCursors);
  if (!archive_->ready()) {
    archive_->~StoreForwardArchive();
    archive_ = nullptr;
    arenaOwner = nullptr;
    error_ = Error::InvalidCapacity;
    return false;
  }
  enabled_ = true;
  error_ = Error::None;
  return true;
#else
  (void)callerReady;
  (void)enabled;
  configuredRecords_ = configuredRecords;
  enabled_ = false;
  error_ = Error::FeatureDisabled;
  return false;
#endif
}

void StoreForwardMemory::disable() {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  if (arenaOwner == this) {
    if (archive_) archive_->clear();
    if (archive_) archive_->~StoreForwardArchive();
    archive_ = nullptr;
    arenaOwner = nullptr;
  }
#endif
  enabled_ = false;
  error_ = Error::Disabled;
}

StoreForwardMemory::Snapshot StoreForwardMemory::snapshot() const {
  Snapshot out{};
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  out.reservedBytes = sizeof(arena);
  out.capacity = configuredRecords_ <= MaxRecords ? configuredRecords_ : 0;
  out.usedRecords = archive_ && enabled_ ? archive_->size() : 0;
  out.freeRecords = out.capacity >= out.usedRecords ? out.capacity - out.usedRecords : 0;
  out.freeBytes = out.freeRecords * sizeof(StoreForwardArchive::Record);
  out.available = arenaOwner == this && archive_ && enabled_ && archive_->ready();
#else
  out.error = Error::FeatureDisabled;
  return out;
#endif
  out.error = error_;
  return out;
}

StoreForwardArchive *StoreForwardMemory::archive() {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  return arenaOwner == this && enabled_ ? archive_ : nullptr;
#else
  return nullptr;
#endif
}

const StoreForwardArchive *StoreForwardMemory::archive() const {
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  return arenaOwner == this && enabled_ ? archive_ : nullptr;
#else
  return nullptr;
#endif
}

}  // namespace mt::modules
