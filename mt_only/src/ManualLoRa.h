// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "LoRaPresets.h"

namespace mt {
// Stock numeric normalization, followed by the current SX126x API's supported
// bandwidths/coding rates. This describes configuration; it sends nothing.
// Adapted from the four M96 Qwen proposals and pinned stock MeshRadio.h.
struct ManualLoRa {
  static float bandwidthKHz(uint32_t raw) {
    const uint16_t code=uint16_t(raw);
    switch(code) {
    case 8:return 7.8f;case 10:return 10.4f;case 16:return 15.6f;
    case 21:return 20.8f;case 31:return 31.25f;case 42:return 41.7f;
    case 62:return 62.5f;case 200:return 203.125f;case 400:return 406.25f;
    case 800:return 812.5f;case 1600:return 1625.0f;
    default:return code?float(code):250.0f;
    }
  }
  static uint8_t spreadingFactor(uint32_t raw) {
    const uint8_t sf=uint8_t(raw);return sf<5||sf>12?11:sf;
  }
  static uint8_t codingDenominator(uint32_t raw) {
    const uint8_t cr=uint8_t(raw);return cr<4||cr>8?5:cr;
  }
  static bool resolve(uint32_t bandwidth,uint32_t sf,uint32_t cr,LoRaPresets::Profile &out) {
    const uint32_t hz=uint32_t(bandwidthKHz(bandwidth)*1000.0f);
    switch(hz) {
    case 7800:case 10400:case 15600:case 20800:case 31250:case 41700:
    case 62500:case 125000:case 250000:case 500000:break;
    default:return false;
    }
    const auto denominator=codingDenominator(cr);
    // Stock accepts denominator4, but Zephyr has no CR4/4 member. Do not
    // forward enum0 to hardware without a separately verified chip contract.
    if(denominator==4)return false;
    out={hz,hz,spreadingFactor(sf),denominator};return true;
  }
  static int regionalPower(int requested,int limit,bool licensed) {
    int power=requested;
    if(!power||(power>limit&&!licensed))power=limit;
    return power?power:17;
  }
};
}
