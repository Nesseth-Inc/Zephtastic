// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "stock/routing/RoutingModule.h"
namespace mt {
// Meshtastic 8e6a88d: Default::getConfiguredOrDefaultHopLimit and
// RoutingModule::getHopLimitForResponse, non-event mode. Response inputs
// here are validated modern headers with a known hop count.
struct HopPolicy {
  static constexpr unsigned Maximum=7,Initial=3;
  static bool valid(unsigned value){return value<=Maximum;}
  static uint8_t configured(uint8_t value,uint8_t stored,uint8_t cap=Maximum){return value>=cap?cap:stored;}
  static unsigned response(unsigned start,unsigned remaining,unsigned stored){
    if(!valid(start)||remaining>start||!valid(stored))return stored;
    return stock::routing::getHopLimitForResponse(start,remaining,stored);
  }
};
}
