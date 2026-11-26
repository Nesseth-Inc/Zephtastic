// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "FixedPartitionBlankScan.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>

namespace mt::m151::spare_status {
using blank_scan::Result;
#if DT_NODE_EXISTS(DT_NODELABEL(mt_pki_history)) && DT_NODE_EXISTS(DT_NODELABEL(lfs_partition))
constexpr size_t Start=DT_REG_ADDR(DT_NODELABEL(lfs_partition));
constexpr size_t Bytes=DT_REG_SIZE(DT_NODELABEL(lfs_partition));
static_assert(Start==0xd4000 && Bytes==blank_scan::kPartitionBytes,
              "Numeric probe is restricted to the fixed T096 LittleFS region");
inline int32_t read(void *,uint32_t offset,uint8_t *out,size_t amount,size_t *actual) {
  if(!out||!actual||!amount||offset>Bytes||amount>Bytes-offset)return -EINVAL;
  const auto *device=FIXED_PARTITION_DEVICE(lfs_partition);
  if(!device||!device_is_ready(device))return -ENODEV;
  const int rc=flash_read(device,Start+offset,out,amount);
  if(!rc)*actual=amount;
  return rc;
}
inline Result scan() {return blank_scan::scan(read,nullptr);}
#else
inline Result scan() {
  return {blank_scan::kPartitionBytes,0,0,0,-ENOTSUP,false};
}
#endif
} // namespace mt::m151::spare_status
