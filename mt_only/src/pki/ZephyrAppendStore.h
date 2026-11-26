// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "AppendStore.h"
#include "LegacyTailTools.h"

namespace mt::pki {
// Only the new mt_pki_history fixed partition is addressable. No old /lfs,
// NVS, SoftDevice, bootloader, UICR or flash-controller reset is exposed.
class ZephyrAppendStore final:public AppendStore {
public:
  size_t capacity()const override;
  int read(size_t,uint8_t *,size_t)override;
  int program(size_t,const uint8_t *,size_t)override;
  int selectCounterMode()override;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  int blank(bool &out); // Read-only status projection; never exports bytes.
#endif
};
}
