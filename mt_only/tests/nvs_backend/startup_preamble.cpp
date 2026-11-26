#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <vector>
#define ADDR_SECT_SHIFT 16
#define ADDR_SECT_MASK 0xffff0000UL
#define ADDR_OFFS_MASK 0x0000ffffUL
#define K_FOREVER 0
#define LOG_INF(...) do {} while (0)
#define LOG_WRN(...) do {} while (0)
struct nvs_ate { uint16_t id, offset, len; uint8_t part, crc8; };
struct flash_parameters { size_t write_block_size; uint8_t erase_value; };
struct nvs_fs {
 int nvs_lock; uint32_t sector_size, sector_count; struct flash_parameters *flash_parameters;
 uint32_t ate_wra, data_wra;
};
static std::vector<int> comparisons;
static size_t comparisonIndex;
static int recoveries, writes, erases, lockDepth;
static std::vector<uint32_t> comparisonAddresses;
static void k_mutex_lock(int *, int) { ++lockDepth; }
static void k_mutex_unlock(int *) { --lockDepth; }
static size_t nvs_al_size(nvs_fs *, size_t n) { return n; }
// The production comparator returns 0 (all erased), 1 (not erased), or negative
// flash-read errno. Script only this hardware boundary, preserving startup logic.
static int nvs_flash_cmp_const(nvs_fs *, uint32_t address, uint8_t, size_t) {
 comparisonAddresses.push_back(address);
 return comparisonIndex < comparisons.size() ? comparisons[comparisonIndex++] : 0;
}
static void nvs_sector_advance(nvs_fs *fs, uint32_t *address) {
 *address = (((*address >> ADDR_SECT_SHIFT) + 1) % fs->sector_count) << ADDR_SECT_SHIFT
            | (*address & ADDR_OFFS_MASK);
}
static int nvs_recover_last_ate(nvs_fs *, uint32_t *address) { ++recoveries; *address -= sizeof(nvs_ate); return 0; }
static int nvs_flash_ate_rd(nvs_fs *, uint32_t, nvs_ate *) { return 0; }
static int nvs_ate_cmp_const(nvs_ate *, uint8_t) { return 0; }
static bool nvs_ate_valid(nvs_fs *, uint32_t, nvs_ate *) { return false; }
static int nvs_flash_erase_sector(nvs_fs *, uint32_t) { ++erases; return 0; }
static int nvs_gc(nvs_fs *, void *) { ++writes; return 0; }
static int nvs_add_gc_done_ate(nvs_fs *) { ++writes; return 0; }
