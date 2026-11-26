// SPDX-License-Identifier: GPL-3.0-only
// Modulation-parameter mapping for Meshtastic LoRa modem presets (IDs 0..16).
// Mirrors stock modemPresetToParams() (MeshRadio.h, stock HEAD
// 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c), kHz floats converted to integer
// Hz. Mapping only: no regional availability, hop limits, tx power, or
// hardware policy. Does not imply the current driver consumes this.
//
// Pure switch; no I/O; no global state.

#pragma once

#include <stdint.h>

namespace mt {

struct LoRaPresets {
  struct Profile {
    uint32_t bandwidthHz;       // standard/sub-GHz (wideLora=false)
    uint32_t wideBandwidthHz;   // wideLora=true
    uint8_t spreadingFactor;
    uint8_t codingDenominator;
  };

  // Fills out and returns true for preset IDs 0..16. Any other raw ID
  // returns false and leaves out byte-for-byte unchanged.
  static bool lookup(unsigned preset, Profile &out) {
    switch (preset) {
      case 0:  out = { 250000,  812500, 11, 5 }; return true; // LONG_FAST
      case 1:  out = { 125000,  406250, 12, 8 }; return true; // LONG_SLOW
      case 2:  out = { 250000,  812500, 11, 5 }; return true; // VERY_LONG_SLOW (stock falls back to LONG_FAST)
      case 3:  out = { 250000,  812500, 10, 5 }; return true; // MEDIUM_SLOW
      case 4:  out = { 250000,  812500,  9, 5 }; return true; // MEDIUM_FAST
      case 5:  out = { 250000,  812500,  8, 5 }; return true; // SHORT_SLOW
      case 6:  out = { 250000,  812500,  7, 5 }; return true; // SHORT_FAST
      case 7:  out = { 125000,  406250, 11, 8 }; return true; // LONG_MODERATE
      case 8:  out = { 500000, 1625000,  7, 5 }; return true; // SHORT_TURBO
      case 9:  out = { 500000, 1625000, 11, 8 }; return true; // LONG_TURBO
      case 10: out = { 125000,  125000,  9, 5 }; return true; // LITE_FAST
      case 11: out = { 125000,  125000, 10, 5 }; return true; // LITE_SLOW
      case 12: out = {  62500,   62500,  7, 6 }; return true; // NARROW_FAST
      case 13: out = {  62500,   62500,  8, 6 }; return true; // NARROW_SLOW
      case 14: out = {  15600,   15600,  7, 5 }; return true; // TINY_FAST
      case 15: out = {  15600,   15600,  8, 6 }; return true; // TINY_SLOW
      case 16: out = { 500000, 1625000,  9, 5 }; return true; // MEDIUM_TURBO
      default: return false;
    }
  }
};

} // namespace mt
