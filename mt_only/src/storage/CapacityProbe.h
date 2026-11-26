// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

namespace mt::m153::capacity_probe {
constexpr uint32_t LfsBytes = 131072u;
constexpr uint32_t SectorBytes = 4096u;
constexpr size_t ChunkBytes = 64u;
constexpr size_t SectorCount = LfsBytes / SectorBytes;
static_assert(SectorCount == 32u, "fixed T096 LFS geometry changed");

enum : int32_t { NoReader = -10001, NoProgress = -10002, BadReadCount = -10003 };
using ReadFn = int32_t (*)(void *, uint32_t, uint8_t *, size_t, size_t *);

struct LfsResult {
  uint32_t bytes_scanned = 0;
  uint16_t ff_bytes[SectorCount]{};
  int32_t error = 0;
  uint8_t sectors_complete = 0;
  bool complete = false;
};

inline void wipe(uint8_t *p, size_t n) {
  volatile uint8_t *v = p;
  while (n--) *v++ = 0;
}

// Bounded fixed-range read. Only numeric FF counts/progress escape; scratch is
// cleared after each callback and on every error path.
inline LfsResult scanLfs(ReadFn read, void *ctx) {
  LfsResult r{};
  uint8_t scratch[ChunkBytes];
  if (!read) { r.error = NoReader; wipe(scratch, sizeof(scratch)); return r; }
  for (uint8_t sector = 0; sector < SectorCount; ++sector) {
    uint32_t within = 0;
    while (within < SectorBytes) {
      const size_t want = (SectorBytes - within < ChunkBytes)
                              ? size_t(SectorBytes - within) : ChunkBytes;
      size_t actual = 0;
      const uint32_t offset = uint32_t(sector) * SectorBytes + within;
      const int32_t rc = read(ctx, offset, scratch, want, &actual);
      if (rc) { wipe(scratch, sizeof(scratch)); r.error = rc; return r; }
      if (actual == 0u) { wipe(scratch, sizeof(scratch)); r.error = NoProgress; return r; }
      if (actual > want) { wipe(scratch, sizeof(scratch)); r.error = BadReadCount; return r; }
      for (size_t i = 0; i < actual; ++i)
        if (scratch[i] == 0xffu) ++r.ff_bytes[sector];
      within += uint32_t(actual);
      r.bytes_scanned += uint32_t(actual);
      wipe(scratch, sizeof(scratch));
    }
    ++r.sectors_complete;
  }
  r.complete = r.bytes_scanned == LfsBytes && r.sectors_complete == SectorCount;
  return r;
}

} // namespace mt::m153::capacity_probe

#if defined(CONFIG_BOARD_HELTEC_T096) && CONFIG_BOARD_HELTEC_T096
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/kvss/nvs.h>

namespace mt::m153::capacity_probe {
constexpr uint32_t LfsStart = DT_REG_ADDR(DT_NODELABEL(lfs_partition));
constexpr uint32_t NvsStart = DT_REG_ADDR(DT_NODELABEL(storage_partition));
constexpr uint32_t NvsBytes = DT_REG_SIZE(DT_NODELABEL(storage_partition));
static_assert(LfsStart == 0xd4000u && DT_REG_SIZE(DT_NODELABEL(lfs_partition)) == LfsBytes,
              "T096 LittleFS geometry changed");
static_assert(NvsStart == 0xd0000u && NvsBytes == 0x4000u,
              "T096 settings NVS geometry changed");

struct NvsResult { int32_t error = 0; int32_t free_bytes = -1; };
// Requires normal SETTINGS_NVS startup. Does not mount/init. nvs_calc_free_space
// is read-only and assumes its caller serializes with fs->nvs_lock.
inline NvsResult existingNvsFreeSpace() {
  void *storage = nullptr;
  const int getRc = settings_storage_get(&storage);
  if (getRc) return {getRc, -1};
  if (!storage) return {-ENODEV, -1};
  auto *fs = static_cast<nvs_fs *>(storage);
  if (!fs->ready || fs->sector_size != SectorBytes || fs->sector_count != 4u)
    return {-EACCES, -1};
  if (k_mutex_lock(&fs->nvs_lock, K_NO_WAIT)) return {-EBUSY, -1};
  const ssize_t freeBytes = fs->ready ? nvs_calc_free_space(fs) : -EACCES;
  k_mutex_unlock(&fs->nvs_lock);
  if (freeBytes < 0) return {int32_t(freeBytes), -1};
  return {0, int32_t(freeBytes)};
}

inline int32_t readLfs(void *, uint32_t offset, uint8_t *out,
                       size_t amount, size_t *actual) {
  if (!out || !actual || !amount || offset > LfsBytes || amount > LfsBytes - offset)
    return -EINVAL;
  const auto *dev = FIXED_PARTITION_DEVICE(lfs_partition);
  if (!dev || !device_is_ready(dev)) return -ENODEV;
  const int rc = flash_read(dev, LfsStart + offset, out, amount);
  if (!rc) *actual = amount;
  return rc;
}
} // namespace mt::m153::capacity_probe
#endif
