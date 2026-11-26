// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "StoreForwardArchive.h"

namespace mt::modules {

// Fixed PSRAM backing store for the optional XIAO Store & Forward archive.
// The owner must serialize calls and provide an independently verified PSRAM
// readiness result before initialize(). No startup probe or work is implicit.
class StoreForwardMemory {
public:
  static constexpr size_t MaxRecords = 400;
  static constexpr size_t MaxCursors = 32;
  static constexpr size_t MaxReservedBytes = 128 * 1024;

  enum class Error : uint8_t {
    None,
    FeatureDisabled,
    Disabled,
    CallerNotReady,
    InvalidCapacity,
    CapacityExceeded,
    Busy,
    CanaryFailed,
    NotInitialized,
  };

  struct Snapshot {
    bool available = false;
    size_t capacity = 0;
    size_t usedRecords = 0;
    size_t freeRecords = 0;
    size_t reservedBytes = 0;
    // Free bytes in record slots only; this excludes cursor/control storage
    // and is not a total-PSRAM measurement.
    size_t freeBytes = 0;
    Error error = Error::NotInitialized;
  };

  StoreForwardMemory() = default;
  StoreForwardMemory(const StoreForwardMemory &) = delete;
  StoreForwardMemory &operator=(const StoreForwardMemory &) = delete;
  ~StoreForwardMemory();

  // CallerReady must come from the owning board startup path. A false value
  // leaves server admission unavailable without touching the arena. Requests
  // above MaxRecords fail whole; capacity is never clipped. The arena has one
  // exclusive owner; calls across instances must be serialized by the caller.
  bool initialize(bool callerReady, bool enabled, size_t configuredRecords);

  // Clears volatile records and cursors but leaves the static PSRAM reservation
  // in place. The storage is volatile and is never persisted.
  void disable();

  Snapshot snapshot() const;
  Error lastError() const { return error_; }
  StoreForwardArchive *archive();
  const StoreForwardArchive *archive() const;

private:
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM) && CONFIG_MT_STORE_FORWARD_PSRAM
  alignas(StoreForwardArchive) uint8_t archiveStorage_[sizeof(StoreForwardArchive)];
#endif
  StoreForwardArchive *archive_ = nullptr;
  size_t configuredRecords_ = 0;
  Error error_ = Error::NotInitialized;
  bool enabled_ = false;
};

static_assert(StoreForwardMemory::MaxRecords == 400, "fixed PSRAM record cap");
static_assert(StoreForwardMemory::MaxCursors == 32, "fixed PSRAM cursor cap");
static_assert(sizeof(StoreForwardArchive::Record) <= 288,
              "archive record budget changed");

}  // namespace mt::modules
