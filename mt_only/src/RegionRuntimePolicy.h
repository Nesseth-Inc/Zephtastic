// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "RegionPolicy.h"
#include "TxAdmission.h"
namespace mt { namespace radio { namespace RegionRuntimePolicy {
// Table presence is not runtime support. These endpoints have no licensed
// owner state or frequency-switching scheduler; UNSET remains silent.
inline TxAdmission::Decision duty(uint32_t regionCode,bool fixedRouter,bool polite,
                                  bool known,float percent) {
  const auto *region=RegionPolicy::lookup(regionCode);
  if(!region||!RegionPolicy::softwareUsableOnSx126x(*region,false,false))
    return TxAdmission::Decision::Unavailable;
  const float limit=RegionPolicy::effectiveDutyPercent(*region,fixedRouter?
    RegionPolicy::DeviceRole::FixedRouter:RegionPolicy::DeviceRole::Mobile);
  // Polite position scheduling retains the existing half-duty margin. The
  // common Router::send limit in pinned stock blocks only above the limit.
  if(polite)return TxAdmission::duty(false,limit,50.0f,known,percent);
  if(!isfinite(limit)||limit<0||limit>100)return TxAdmission::Decision::Invalid;
  if(limit==100)return TxAdmission::Decision::Allow;
  if(!known)return TxAdmission::Decision::Unavailable;
  if(!isfinite(percent)||percent<0||percent>100)return TxAdmission::Decision::Invalid;
  return percent>limit?TxAdmission::Decision::Block:TxAdmission::Decision::Allow;
}
} } }
