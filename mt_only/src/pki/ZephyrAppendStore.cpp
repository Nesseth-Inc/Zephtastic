// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrAppendStore.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include <string.h>

namespace mt::pki {
namespace {
#if defined(CONFIG_BOARD_HELTEC_T096)
constexpr size_t ApplicationEnd = DT_REG_ADDR(DT_NODELABEL(code_partition)) +
                                  DT_REG_SIZE(DT_NODELABEL(code_partition));
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
#if defined(CONFIG_MT_T096_FULL_COMPANION)
#if defined(CONFIG_MT_T096_PAGED_LAYOUT)
constexpr size_t ExpectedAppEnd=0xdd000,ExpectedLfsStart=0xe1000;
#else
constexpr size_t ExpectedAppEnd=0xdc000,ExpectedLfsStart=0xe0000;
#endif
static_assert(DT_NODE_EXISTS(DT_NODELABEL(mt_reset_floor))&&DT_NODE_EXISTS(DT_NODELABEL(mt_sat_fs)));
#else
constexpr size_t ExpectedAppEnd=0xd0000,ExpectedLfsStart=0xd4000;
#endif
static_assert(DT_REG_ADDR(DT_NODELABEL(code_partition))==0x26000 &&
              !DT_NODE_EXISTS(DT_NODELABEL(mt_pki_history)) && ApplicationEnd == ExpectedAppEnd,
              "fresh-only T096 requires its dedicated extended application layout");
static_assert(DT_REG_ADDR(DT_NODELABEL(storage_partition)) == ApplicationEnd &&
              DT_REG_SIZE(DT_NODELABEL(storage_partition)) == 0x4000 &&
              DT_REG_ADDR(DT_NODELABEL(lfs_partition)) == ExpectedLfsStart,
              "T096 NVS and LittleFS must match the selected first-install layout");
#if DT_NODE_EXISTS(DT_NODELABEL(mt_reset_floor))
static_assert(DT_REG_SIZE(DT_NODELABEL(lfs_partition)) == 0xf3000-ExpectedLfsStart &&
              DT_REG_ADDR(DT_NODELABEL(mt_reset_floor)) == 0xf3000 &&
              DT_REG_SIZE(DT_NODELABEL(mt_reset_floor)) == 0x1000 &&
              DT_REG_ADDR(DT_NODELABEL(mt_reset_floor)) + DT_REG_SIZE(DT_NODELABEL(mt_reset_floor)) == 0xf4000,
              "T096 reset enrollment uses only the last page below UF2");
#elif DT_NODE_EXISTS(DT_NODELABEL(mt_sat_fs))
static_assert(DT_REG_SIZE(DT_NODELABEL(lfs_partition)) == 0x1f000 &&
              DT_REG_ADDR(DT_NODELABEL(lfs_partition)) + DT_REG_SIZE(DT_NODELABEL(lfs_partition)) == 0xf3000,
              "private satellite volume preserves the retirement page at 0xf3000");
#else
static_assert(DT_REG_SIZE(DT_NODELABEL(lfs_partition)) == 0x20000,
              "ordinary T096 profiles preserve the entire legacy LittleFS range");
#endif
#else
static_assert(DT_NODE_EXISTS(DT_NODELABEL(mt_pki_history)) && ApplicationEnd == 0xa6000,
              "existing T096 history may only be reclaimed by the fresh-only profile");
#endif
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(mt_pki_history))
constexpr size_t Bytes=DT_REG_SIZE(DT_NODELABEL(mt_pki_history));
constexpr size_t Start=DT_REG_ADDR(DT_NODELABEL(mt_pki_history));
static_assert(Start==0xa6000&&Bytes==172032,"M70 dedicated history geometry");
const device *flash(){return FIXED_PARTITION_DEVICE(mt_pki_history);}
#else
constexpr size_t Bytes=0,Start=0;
const device *flash(){return nullptr;}
#endif
bool range(size_t offset,size_t n){return n&&offset<=Bytes&&n<=Bytes-offset;}
}
size_t ZephyrAppendStore::capacity()const{return Bytes;}
int ZephyrAppendStore::read(size_t offset,uint8_t *out,size_t n){
  if(!out||!range(offset,n))return -EINVAL;
  const auto *dev=flash();if(!dev||!device_is_ready(dev))return -ENODEV;
  return flash_read(dev,Start+offset,out,n);
}
int ZephyrAppendStore::program(size_t offset,const uint8_t *data,size_t n){
  if(!data||!range(offset,n)||(offset&3)||(n&3))return -EINVAL;
  // Every program is into previously erased bytes. This adapter has no erase
  // call, including on a nonblank refusal or a partially completed program.
  uint8_t before[64];
  for(size_t at=0;at<n;){const size_t amount=n-at<sizeof(before)?n-at:sizeof(before);
    const int rc=read(offset+at,before,amount);if(rc)return rc;
    for(size_t i=0;i<amount;++i)if(before[i]!=0xff)return -EEXIST;
    at+=amount;
  }
  return flash_write(flash(),Start+offset,data,n);
}
int ZephyrAppendStore::selectCounterMode(){
  if(Bytes!=172032)return -ENOTSUP;
  constexpr size_t Offset=4092;
  constexpr uint8_t Before[4]={'C','M','I','T'},After[4]={'B','M','I','T'};
  uint8_t word[4];int rc=read(Offset,word,sizeof(word));if(rc)return rc;
  if(memcmp(word,Before,sizeof(word)))return -EEXIST;
  // Only bit0 changes 1->0. Ordinary programs retain their erased-only guard.
  // The coordinator forbids a second attempt after staged nonce records exist.
  rc=flash_write(flash(),Start+Offset,After,sizeof(After));if(rc)return rc;
  rc=read(Offset,word,sizeof(word));return rc?rc:memcmp(word,After,sizeof(word))?-EIO:0;
}
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
int ZephyrAppendStore::blank(bool &out){
  if(Bytes!=172032)return -ENOTSUP;
  uint8_t bytes[64];bool result=true;
  for(size_t offset=0;offset<Bytes;offset+=sizeof(bytes)){
    const int rc=read(offset,bytes,sizeof(bytes));if(rc)return rc;
    for(uint8_t b:bytes)if(b!=0xff)result=false;
  }
  out=result;return 0;
}
#endif
}
