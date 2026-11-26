// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "RegionPolicy.h"

#include <math.h>
#include <stdint.h>

namespace mt {

struct USChannelSelection {
  uint16_t slotCount;
  uint16_t slot;
  uint32_t frequencyHz;
  uint32_t regionCode;
};

class USChannel {
  static uint32_t hashName(const char *name) {
    uint32_t hash = 5381;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
      hash = hash * 33u + *p;
    return hash;
  }

  static bool hashSlot(const char *name, uint32_t count, uint32_t &slot) {
    if (!name || !count) return false;
    slot = hashName(name) % count;
    return true;
  }

 public:
  // Stock RadioInterface.cpp frequency-slot calculation. Frequencies remain
  // float MHz through the stock formula; only the native boundary rounds to Hz.
  // channelNum is stock's 1-based channel number, with zero selecting the
  // region's configured hash/explicit default. A nonzero frequency override
  // takes precedence over channelNum and receives the configured offset.
  static bool lookupStock(const radio::RegionPolicy::Region &region,
                          float bandwidthKHz, uint32_t channelNum,
                          const char *channelName, const char *presetDisplayName,
                          float overrideMHz, float offsetMHz,
                          USChannelSelection &out) {
    using namespace radio::RegionPolicy;
    if (!region.profile || !channelName || !isfinite(bandwidthKHz) || bandwidthKHz <= 0.0f ||
        !isfinite(region.freqStartMHz) || !isfinite(region.freqEndMHz) ||
        !isfinite(region.profile->spacingMHz) || !isfinite(region.profile->paddingMHz) ||
        !isfinite(overrideMHz) || !isfinite(offsetMHz) ||
        region.freqEndMHz <= region.freqStartMHz)
      return false;

    // Match stock's float-MHz arithmetic and rounded slot count.
    const float freqSlotWidth = region.profile->spacingMHz +
        (region.profile->paddingMHz * 2.0f) + (bandwidthKHz / 1000.0f);
    if (!isfinite(freqSlotWidth) || freqSlotWidth <= 0.0f) return false;
    const float roundedCount = round((region.freqEndMHz - region.freqStartMHz +
                                      region.profile->spacingMHz) / freqSlotWidth);
    if (!isfinite(roundedCount) || roundedCount < 1.0f || roundedCount > 65535.0f)
      return false;
    const uint32_t count = static_cast<uint32_t>(roundedCount);

    uint32_t channelNameHashSlot = 0;
    if (!hashSlot(channelName, count, channelNameHashSlot)) return false;
    uint32_t presetNameHashSlot = 0;
    if (region.overrideSlot == -1 &&
        !hashSlot(presetDisplayName, count, presetNameHashSlot))
      return false;

    const bool usesDefaultSlot = channelNum == 0 ||
        (region.overrideSlot > 0 && channelNum == static_cast<uint32_t>(region.overrideSlot)) ||
        (region.overrideSlot == 0 && channelNum > 0 && channelNum - 1 == channelNameHashSlot) ||
        (region.overrideSlot == -1 && channelNum > 0 && channelNum - 1 == presetNameHashSlot);

    float centreMHz = 0.0f;
    uint16_t selectedSlot = UINT16_MAX;
    if (overrideMHz != 0.0f) {
      centreMHz = overrideMHz;
    } else {
      if (channelNum > count) return false;
      uint32_t slot = 0;
      if (usesDefaultSlot) {
        if (region.overrideSlot > 0)
          slot = static_cast<uint32_t>(region.overrideSlot - 1);
        else if (region.overrideSlot == -1)
          slot = presetNameHashSlot;
        else
          slot = channelNameHashSlot;
      } else {
        slot = channelNum - 1;
      }
      if (slot >= count) return false;
      selectedSlot = static_cast<uint16_t>(slot);
      // Keep the expression and operation order aligned with stock:
      // band edge + half bandwidth + padding + slot * slot width.
      centreMHz = region.freqStartMHz + (bandwidthKHz / 2000.0f) +
          region.profile->paddingMHz + (slot * freqSlotWidth);
    }

    centreMHz += offsetMHz;
    if (!isfinite(centreMHz) || centreMHz < region.freqStartMHz ||
        centreMHz > region.freqEndMHz)
      return false;
    const double hz = double(centreMHz) * 1000000.0;
    if (!isfinite(hz) || hz < 0.0 || hz > double(UINT32_MAX)) return false;
    const USChannelSelection candidate{static_cast<uint16_t>(count), selectedSlot,
                                       static_cast<uint32_t>(llround(hz)), region.code};
    out = candidate;
    return true;
  }

  // Legacy US stock-style API retained as a wrapper around the regional math.
  static bool lookupStock(float bandwidthKHz, uint32_t channelNum,
                          const char *name, float overrideMHz, float offsetMHz,
                          USChannelSelection &out) {
    const auto *region = radio::RegionPolicy::lookup(1); // US
    if (!region) return false;
    USChannelSelection candidate{};
    if (lookupStock(*region, bandwidthKHz, channelNum, name, name,
                    overrideMHz, offsetMHz, candidate)) {
      out = candidate;
      return true;
    }
    // The historical ZephCore wrapper used a hashed slot when channelNum was
    // outside the computed US range; Config::setRadio then persisted the
    // bounded replacement. Preserve that US-only migration behavior while the
    // generic regional resolver remains strict for new candidates.
    if (channelNum == 0 || overrideMHz != 0.0f) return false;
    if (!lookupStock(*region, bandwidthKHz, 0, name, name,
                     overrideMHz, offsetMHz, candidate))
      return false;
    out = candidate;
    return true;
  }

  // Exact legacy US PROFILE_STD calculation for callers that already have Hz.
  static bool lookup(uint32_t bandwidthHz, uint32_t channelNum,
                     const char *primaryName, USChannelSelection &out) {
    if (!primaryName || (bandwidthHz != 125000 && bandwidthHz != 250000 &&
                         bandwidthHz != 500000))
      return false;
    const uint32_t count = 26000000u / bandwidthHz;
    if (channelNum > count) return false;
    const uint32_t slot = channelNum ? channelNum - 1 : hashName(primaryName) % count;
    const USChannelSelection candidate{
        static_cast<uint16_t>(count), static_cast<uint16_t>(slot),
        902000000u + bandwidthHz / 2 + slot * bandwidthHz, 1};
    out = candidate;
    return true;
  }
};

} // namespace mt
