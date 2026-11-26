// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <limits.h>
#include <float.h>
namespace mt { namespace ModuleIntervals {
// Ordered bounds also support Zephyr minimal C++ without <cmath>.
// Pinned stock Default seconds conversion and role defaults. Pure policy;
// the caller supplies an observed online count and resolved modem parameters.
inline uint32_t base(uint32_t configuredSeconds,bool router) {
  const uint32_t seconds=configuredSeconds?configuredSeconds:(router?43200u:3600u);
  return seconds>uint32_t(INT32_MAX)/1000u?uint32_t(INT32_MAX):seconds*1000u;
}
inline bool scale(uint32_t baseMs,uint32_t online,unsigned sf,float bwKHz,
                  bool event,bool bypass,uint32_t &out) {
  if(baseMs>uint32_t(INT32_MAX)||online>uint32_t(INT32_MAX))return false;
  if(bypass||online<=40){out=baseMs;return true;}
  if(sf<5||sf>12||!(bwKHz>0&&bwKHz<=FLT_MAX))return false;
  const float factor=float(uint32_t(1)<<sf)/(bwKHz*(event?25.0f:100.0f));
  if(!(factor>=0&&factor<=FLT_MAX))return false;
  const float coefficient=float(1.0+(int(online-40)*factor));
  if(!(coefficient>=1&&coefficient<=FLT_MAX))return false;
  const double wide=double(baseMs)*double(coefficient);
  if(wide>=double(INT32_MAX)){out=uint32_t(INT32_MAX);return true;}
  const float rounded=float(baseMs)*coefficient;
  // A rounded boundary may reach 2^31 even if the double precheck did not.
  if(!(rounded>=0&&rounded<=FLT_MAX)||rounded>=float(INT32_MAX)){out=uint32_t(INT32_MAX);return true;}
  out=uint32_t(rounded);return true;
}
inline uint32_t throttle(uint32_t milliseconds,int8_t multiplier) {
  const uint32_t capped=milliseconds>uint32_t(INT32_MAX)?uint32_t(INT32_MAX):milliseconds;
  if(multiplier<=1)return capped;
  const uint64_t wide=uint64_t(capped)*uint64_t(multiplier);
  return wide>=uint64_t(INT32_MAX)?uint32_t(INT32_MAX):uint32_t(wide);
}
inline bool device(uint32_t configuredSeconds,bool router,bool onlineKnown,
                   uint32_t online,unsigned sf,float bwKHz,bool event,
                   int8_t regionalThrottle,uint32_t &out) {
  uint32_t milliseconds=base(configuredSeconds,router);
  // Routers do not consult online/modem data for this stock policy.
  if(!router&&(!onlineKnown||!scale(milliseconds,online,sf,bwKHz,event,false,milliseconds)))return false;
  out=throttle(milliseconds,regionalThrottle);return true;
}
} }
