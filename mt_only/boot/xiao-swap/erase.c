/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#define BLOCK_BYTES 0x10000U

BUILD_ASSERT(CONFIG_MCUBOOT_LOGICAL_SECTOR_SIZE == BLOCK_BYTES);
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUBOOT_VERIFY_LOGICAL_SECTORS));
BUILD_ASSERT(PARTITION_SIZE(scratch_partition) == BLOCK_BYTES);
BUILD_ASSERT(PARTITION_OFFSET(scratch_partition) % BLOCK_BYTES == 0);
BUILD_ASSERT(PARTITION_OFFSET(slot0_partition) % BLOCK_BYTES == 0);
BUILD_ASSERT(PARTITION_OFFSET(slot1_partition) % BLOCK_BYTES == 0);

int __real_boot_erase_region(const struct flash_area *area, uint32_t offset,
                             uint32_t size, bool backwards);

int __wrap_boot_erase_region(const struct flash_area *area, uint32_t offset,
                             uint32_t size, bool backwards)
{
    /* MCUboot retires authoritative trailers from the end in physical-page
     * order for power-loss recovery. Only coalesce a complete forward erase
     * of a scratch-swap destination. Slot trailer blocks use upstream too.
     */
    if (!backwards && size == BLOCK_BYTES && offset % BLOCK_BYTES == 0 &&
        offset < area->fa_size && size <= area->fa_size - offset) {
        const bool scratch = area->fa_id == PARTITION_ID(scratch_partition);
        const bool slot = area->fa_id == PARTITION_ID(slot0_partition) ||
                          area->fa_id == PARTITION_ID(slot1_partition);
        if (scratch || (slot && area->fa_size - offset > BLOCK_BYTES)) {
            return flash_area_erase(area, offset, size);
        }
    }
    return __real_boot_erase_region(area, offset, size, backwards);
}
