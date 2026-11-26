// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "stock/routing/RoutingModule.h"
namespace mt {
// Callers that already validated a modern header can omit the bitfield flag.
// Legacy or opaque frames with zero hop_start have an unknown hop distance.
struct HopPolicy {
  static constexpr unsigned Maximum=7,Initial=3;
  static bool valid(unsigned value){return value<=Maximum;}
  static uint8_t configured(uint8_t value,uint8_t stored,uint8_t cap=Maximum){return value>=cap?cap:stored;}
  static unsigned response(unsigned start,unsigned remaining,unsigned stored,
                           bool decodedHasBitfield=true){
    return stock::routing::getHopLimitForResponse(start,remaining,stored,decodedHasBitfield);
  }
};
}
