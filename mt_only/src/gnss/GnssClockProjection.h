// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "GnssPromotionProjection.h"
namespace zt::gnss {
// Local clock observation from the same promoted, fresh fix. Independent of
// optional RF timestamp flags; no OS/RTC, persistence or scheduling changes.
inline uint32_t projectUtc(const PromotionEvent &fix,int64_t now,int64_t maxAge){
  meshtastic_Position checked{};
  if(!fix.measured||!fix.hasUtc||!fix.utcSeconds||!projectPosition(fix,now,maxAge,checked))return 0;
  const uint64_t elapsed=uint64_t(now-fix.capturedUptimeMs)/1000u;
  if(elapsed>UINT32_MAX-fix.utcSeconds)return 0;
  return fix.utcSeconds+uint32_t(elapsed);
}
}
