// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Test-only explicit logged-channel clock. Known idle is fixture input, not RF
// silence or a production fallback. Keep this object alive longer than Service.
#include "Service.h"
#include <cassert>
#include <cstdint>
namespace mt { namespace test {
class KnownChannelFixtureClock {
 uint64_t milliseconds_=0;
 static uint64_t read(void *ctx){return ctx?static_cast<KnownChannelFixtureClock *>(ctx)->milliseconds_:UINT64_MAX;}
public:
 uint64_t now()const{return milliseconds_;}
 bool set(uint64_t milliseconds){
  if(milliseconds==UINT64_MAX||milliseconds<milliseconds_)return false;
  milliseconds_=milliseconds;return true;
 }
 void bind(Service &service){
  const bool installed=service.channelMeter().bind({read,this});
  assert(installed);(void)installed;
  service.channelMeter().supported(true); // Explicit simulated native support.
 }
};
} }
