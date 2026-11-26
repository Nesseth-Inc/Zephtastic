// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt {
// One global reply window. Only an accepted reply consumes it, including at0.
class NeighborReplyWindow {
  uint32_t acceptedAt_=0;bool accepted_=false;
public:
  bool allowed(uint32_t now)const{return !accepted_||uint32_t(now-acceptedAt_)>=180000u;}
  void accepted(uint32_t now){accepted_=true;acceptedAt_=now;}
  void reset(){accepted_=false;acceptedAt_=0;}
};
}
