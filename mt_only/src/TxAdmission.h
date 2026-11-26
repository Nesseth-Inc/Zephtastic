// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <math.h>

namespace mt {
class TxAdmission {
 public:
  enum class Decision:uint8_t {Unavailable,Allow,Block,Invalid};
  static Decision channel(bool known,float percent,bool polite) {
    if(!known)return Decision::Unavailable;
    if(!isfinite(percent)||percent<0||percent>100)return Decision::Invalid;
    return percent<(polite?25.0f:40.0f)?Decision::Allow:Decision::Block;
  }
  static Decision duty(bool overrideDuty,float effectiveDuty,float margin,
                       bool known,float percent) {
    if(!isfinite(effectiveDuty)||effectiveDuty<0||effectiveDuty>100||
       !isfinite(margin)||margin<0||margin>100)return Decision::Invalid;
    if(overrideDuty||effectiveDuty==100)return Decision::Allow;
    if(!known)return Decision::Unavailable;
    if(!isfinite(percent)||percent<0||percent>100)return Decision::Invalid;
    return percent<effectiveDuty*margin/100?Decision::Allow:Decision::Block;
  }
};
}
