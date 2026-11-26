// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "FormerCodePreparation.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
namespace mt::pki {
// Separate from AppendStore: no generic address, program or mass-erase API.
class ZephyrHistoryPageEraser final:public HistoryPageEraser {
#if DT_NODE_EXISTS(DT_NODELABEL(mt_pki_history))
  static constexpr size_t Start=DT_REG_ADDR(DT_NODELABEL(mt_pki_history));
  static constexpr size_t Bytes=DT_REG_SIZE(DT_NODELABEL(mt_pki_history));
  static_assert(Start==0xa6000&&Bytes==172032,"fixed former-code history geometry");
  static const device *flash(){return FIXED_PARTITION_DEVICE(mt_pki_history);}
#else
  static constexpr size_t Start=0,Bytes=0;
  static const device *flash(){return nullptr;}
#endif
public:
  int validate()const override{
    const auto *dev=flash();if(Bytes!=172032||!dev||!device_is_ready(dev))return -ENODEV;
    for(unsigned page=0;page<42;++page){
      flash_pages_info info{};const int rc=flash_get_page_info_by_offs(dev,Start+page*4096,&info);
      if(rc)return rc<0?rc:-EIO;
      if(info.start_offset!=off_t(Start+page*4096)||info.size!=4096)return -ENOTSUP;
    }
    return 0;
  }
  int erasePage(unsigned page)override{
    if(page>=42)return -EINVAL;
    const int rc=validate();if(rc)return rc;
    return flash_erase(flash(),Start+page*4096,4096);
  }
};
}
