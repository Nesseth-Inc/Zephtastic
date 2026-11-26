// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "DeviceResetFloor.h"
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#if DT_NODE_EXISTS(DT_NODELABEL(mt_reset_floor)) && defined(CONFIG_MT_PKI_TARGET_RUNTIME)
#define MT_HAS_DEVICE_RESET 1
#include "DeviceResetBonds.h"
namespace mt::stock::admin::device_reset {
class ZephyrFloor {
  static constexpr unsigned Base=FIXED_PARTITION_OFFSET(mt_reset_floor);
  static_assert(FIXED_PARTITION_SIZE(mt_reset_floor)==Floor::PageBytes);
  static const device *dev(){return FIXED_PARTITION_DEVICE(mt_reset_floor);}
public:
  int read(size_t at,uint8_t *out,size_t n){
    if(at>Floor::PageBytes||n>Floor::PageBytes-at)return -EINVAL;
    return device_is_ready(dev())?flash_read(dev(),Base+at,out,n):-ENODEV;
  }
  int program(size_t at,const uint8_t *bytes,size_t n){
    if(at>Floor::PageBytes||n>Floor::PageBytes-at||!device_is_ready(dev()))return -EINVAL;
    const size_t block=flash_get_write_block_size(dev());
    if(!block||16%block||at%block||n%block)return -ENOTSUP;
    return flash_write(dev(),Base+at,bytes,n);
  }
  // This operation only reads. An earlier firmware, filesystem or unknown
  // owner may have occupied a new profile's declared page.
  int firstEnrollment(bool partial=false){
    unsigned origin=Base,span=Floor::PageBytes;
#if defined(CONFIG_BOARD_HELTEC_T096)
#if defined(CONFIG_MT_T096_PAGED_LAYOUT)
    constexpr unsigned lfsStart=0xe1000;
#elif defined(CONFIG_MT_T096_FULL_COMPANION)
    constexpr unsigned lfsStart=0xe0000;
#else
    constexpr unsigned lfsStart=0xd4000;
#endif
    static_assert(Base==0xf3000 && FIXED_PARTITION_OFFSET(lfs_partition)==lfsStart &&
                  FIXED_PARTITION_SIZE(lfs_partition)==Base-lfsStart);
    origin=lfsStart;span=Base+Floor::PageBytes-lfsStart;
#endif
    uint8_t scratch[64];
    if(!device_is_ready(dev()))return -ENODEV;
    // Floor::recoverTail separately checks the complete independent page.
    // Retrying its first record must also respect an occupied old filesystem.
    if(partial)span-=Floor::PageBytes;
    for(unsigned i=0;i<span;i+=sizeof(scratch)){
      const int rc=flash_read(dev(),origin+i,scratch,sizeof(scratch));if(rc)return rc;
      for(uint8_t b:scratch)if(b!=0xff)return -EEXIST;
    }
    return 0;
  }
};
}
#else
#define MT_HAS_DEVICE_RESET 0
#endif

#if !MT_HAS_DEVICE_RESET && (defined(CONFIG_BOARD_HELTEC_T096) || DT_NODE_EXISTS(DT_NODELABEL(mt_reset_floor)))
#define MT_CHECK_PRIOR_DEVICE_RESET 1
namespace mt::stock::admin::device_reset {
inline int priorEnrollment(){
#if DT_NODE_EXISTS(DT_NODELABEL(mt_reset_floor))
  const device *flash=FIXED_PARTITION_DEVICE(mt_reset_floor);
  constexpr unsigned offset=FIXED_PARTITION_OFFSET(mt_reset_floor);
#else
  const device *flash=FIXED_PARTITION_DEVICE(lfs_partition);
  constexpr unsigned offset=0xf3000;
  static_assert(FIXED_PARTITION_OFFSET(lfs_partition)==0xd4000);
#if DT_NODE_EXISTS(DT_NODELABEL(mt_sat_fs))
  static_assert(FIXED_PARTITION_SIZE(lfs_partition)==0x1f000 &&
                FIXED_PARTITION_OFFSET(lfs_partition)+FIXED_PARTITION_SIZE(lfs_partition)==offset);
#else
  static_assert(FIXED_PARTITION_SIZE(lfs_partition)==0x20000 && offset+Floor::PageBytes==0xf4000);
#endif
#endif
  uint8_t header[8];if(!device_is_ready(flash))return -ENODEV;
  const int rc=flash_read(flash,offset,header,sizeof(header));
  return rc?rc:Floor::enrollmentHeader(header)?-EXDEV:0;
}
}
#else
#define MT_CHECK_PRIOR_DEVICE_RESET 0
#endif
