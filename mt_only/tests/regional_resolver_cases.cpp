// SPDX-License-Identifier: GPL-3.0-only
// Differential stock MHz-slot arithmetic and fail-closed resolver policy.
#include "RadioSettings.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <initializer_list>
#include <iostream>

using mt::LoRaPresets;
using mt::RadioSettings;
using mt::USChannel;
using mt::USChannelSelection;
using mt::radio::RegionPolicy::Region;
using mt::radio::RegionPolicy::RegionProfile;

static void group(const char *name) { std::cout << "PASS " << name << '\n'; }

static uint32_t stockHash(const char *text) {
  uint32_t hash = 5381;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
    hash = hash * 33u + *p;
  return hash;
}

// Literal translation of pinned RadioInterface.cpp lines 1195-1234 and
// 1349-1394. This is a test oracle, not production logic.
static bool stockOracle(const Region &region, float bwKHz, uint32_t channel,
                       const char *channelName, const char *presetName,
                       float overrideMHz, float offsetMHz,
                       USChannelSelection &out) {
  const float slotWidth = region.profile->spacingMHz +
      (region.profile->paddingMHz * 2.0f) + (bwKHz / 1000.0f);
  const uint32_t count = static_cast<uint32_t>(round(
      (region.freqEndMHz - region.freqStartMHz + region.profile->spacingMHz) / slotWidth));
  if (!count || count > UINT16_MAX) return false;
  const uint32_t channelHashSlot = stockHash(channelName) % count;
  const uint32_t presetHashSlot = stockHash(presetName) % count;
  const bool useDefault = channel == 0 ||
      (region.overrideSlot > 0 && channel == static_cast<uint32_t>(region.overrideSlot)) ||
      (region.overrideSlot == 0 && channel > 0 && channel - 1 == channelHashSlot) ||
      (region.overrideSlot == -1 && channel > 0 && channel - 1 == presetHashSlot);
  float frequency = 0.0f;
  uint16_t slot = UINT16_MAX;
  if (overrideMHz != 0.0f) {
    frequency = overrideMHz;
  } else {
    if (channel > count) return false;
    uint32_t selected = 0;
    if (useDefault) {
      if (region.overrideSlot > 0)
        selected = static_cast<uint32_t>(region.overrideSlot - 1);
      else if (region.overrideSlot == -1)
        selected = presetHashSlot;
      else
        selected = channelHashSlot;
    } else {
      selected = channel - 1;
    }
    if (selected >= count) return false;
    slot = static_cast<uint16_t>(selected);
    frequency = region.freqStartMHz + (bwKHz / 2000.0f) +
        region.profile->paddingMHz + (selected * slotWidth);
  }
  frequency += offsetMHz;
  if (!isfinite(frequency) || frequency < region.freqStartMHz ||
      frequency > region.freqEndMHz)
    return false;
  const USChannelSelection candidate{static_cast<uint16_t>(count), slot,
      static_cast<uint32_t>(llround(double(frequency) * 1000000.0)), region.code};
  out = candidate;
  return true;
}

static bool same(const USChannelSelection &a, const USChannelSelection &b) {
  return a.slotCount == b.slotCount && a.slot == b.slot &&
      a.frequencyHz == b.frequencyHz && a.regionCode == b.regionCode;
}

int main() {
  // Every listed stock region exercises every major spacing family at several
  // widths, on hash-default and explicit 1-based slots.
  constexpr float widths[] = {7.8f, 15.6f, 62.5f, 125.0f, 250.0f, 500.0f};
  for (const Region &region : mt::radio::RegionPolicy::detail::kRegions) {
    for (float bw : widths) {
      USChannelSelection expected{}, actual{};
      const bool expectedOk = stockOracle(region, bw, 0, "RegionalName", "MediumFast", 0, 0, expected);
      const bool actualOk = USChannel::lookupStock(region, bw, 0, "RegionalName", "MediumFast",
                                                    0, 0, actual);
      assert(actualOk == expectedOk);
      if (expectedOk) assert(same(actual, expected));
      if (expected.slotCount > 1) {
        const uint32_t channel = expected.slotCount;
        const bool expectedLastOk = stockOracle(region, bw, channel, "RegionalName", "MediumFast",
                                                0, 0, expected);
        const bool actualLastOk = USChannel::lookupStock(region, bw, channel, "RegionalName",
                                                         "MediumFast", 0, 0, actual);
        assert(actualLastOk == expectedLastOk);
        if (expectedLastOk) assert(same(actual, expected));
      }
    }
  }
  group("regional slot count, spacing, padding, hashes, one-based slots and MHz rounding match stock across every region row");

  // Preserve exact legacy US behavior through both old overloads.
  for (uint32_t bw : {125000u, 250000u, 500000u}) {
    const uint32_t count = 26000000u / bw;
    for (uint32_t ch = 0; ch <= count; ++ch) {
      USChannelSelection legacy{}, regional{};
      assert(USChannel::lookup(bw, ch, "LongFast", legacy));
      assert(USChannel::lookupStock(float(bw) / 1000.0f, ch, "LongFast", 0, 0,
                                    regional));
      assert(same(legacy, regional) && legacy.regionCode == 1);
    }
  }
  group("all legacy US channel centers and US stock wrapper remain identical");

  // Stock's special overrideSlot semantics: channel-name hash, preset-name
  // hash, and explicit 1-based slot. Synthetic rows exercise -1 because the
  // pinned current table uses 0/+slot values.
  RegionProfile standard = *mt::radio::RegionPolicy::lookup(1)->profile;
  Region presetHashRegion = *mt::radio::RegionPolicy::lookup(1);
  presetHashRegion.overrideSlot = -1;
  USChannelSelection got{}, want{};
  assert(stockOracle(presetHashRegion, 250.0f, 0, "Channel", "NarrowFast", 0, 0, want));
  assert(USChannel::lookupStock(presetHashRegion, 250.0f, 0, "Channel",
                                "NarrowFast", 0, 0, got));
  assert(same(got, want));
  assert(got.slot == stockHash("NarrowFast") % got.slotCount);
  const uint32_t matchingPresetChannel = got.slot + 1;
  assert(USChannel::lookupStock(presetHashRegion, 250.0f, matchingPresetChannel,
                                "OtherChannel", "NarrowFast", 0, 0, got));
  assert(got.slot == stockHash("NarrowFast") % got.slotCount);
  Region explicitRegion = *mt::radio::RegionPolicy::lookup(1);
  explicitRegion.overrideSlot = 26;
  assert(USChannel::lookupStock(explicitRegion, 250.0f, 0, "Other", "LongFast", 0, 0, got));
  assert(got.slot == 25);
  assert(USChannel::lookupStock(explicitRegion, 250.0f, 26, "Other", "LongFast", 0, 0, got));
  assert(got.slot == 25);
  assert(USChannel::lookupStock(explicitRegion, 250.0f, 1, "Other", "LongFast", 0, 0, got));
  assert(got.slot == 0);
  (void)standard;
  group("channel hash, preset hash and explicit override slot match stock selection rules");

  // Real RegionPolicy-backed resolver rejects unsupported classes before
  // computing a candidate and keeps both outputs atomic on every refusal.
  constexpr uint32_t rejectedRegions[] = {0, 13, 15, 27, 30, 31, 33, 34, 35, 36, 37, UINT32_MAX};
  for (uint32_t code : rejectedRegions) {
    RadioSettings::Request request;
    request.regionCode = code;
    LoRaPresets::Profile profile{1, 2, 3, 4};
    const LoRaPresets::Profile beforeProfile = profile;
    USChannelSelection selection{5, 6, 7, 8};
    const USChannelSelection beforeSelection = selection;
    assert(!RadioSettings::resolve(request, profile, selection, "LongFast", "LongFast"));
    assert(!memcmp(&profile, &beforeProfile, sizeof(profile)));
    assert(same(selection, beforeSelection));
  }
  for (uint32_t code : {2u, 17u, 18u, 27u}) {
    uint32_t normalized = 99;
    const bool allowed = code == 2 || code == 18;
    assert(RadioSettings::normalizeRegionPreset(code, true, 0, normalized) == allowed);
  }
  group("UNSET, unmapped, wide, licensed-only and frequency-switching regions refuse atomically");

  // EU preset ownership must normalize before Config persistence; returned
  // selection contains the final stock region code used for its frequency.
  const struct SwapCase { uint32_t region, preset, selected; } swaps[] = {
      {3, 10, 29}, {3, 13, 32}, {29, 0, 3}, {29, 12, 32},
      {32, 10, 29}, {32, 0, 3}};
  for (const auto &item : swaps) {
    RadioSettings::Request request;
    request.regionCode = item.region;
    request.preset = item.preset;
    request.channel = 0;
    LoRaPresets::Profile profile{};
    USChannelSelection selection{};
    const char *presetName = item.preset == 10 ? "LiteFast" :
        item.preset == 12 ? "NarrowFast" : item.preset == 13 ? "NarrowSlow" : "LongFast";
    assert(RadioSettings::resolve(request, profile, selection, "Channel", presetName));
    assert(selection.regionCode == item.selected);
    assert(profile.bandwidthHz != 0 && selection.slotCount != 0);
  }
  uint32_t unchangedRegion = 77;
  assert(!RadioSettings::normalizeRegionPreset(3, true, UINT32_MAX, unchangedRegion));
  assert(unchangedRegion == 77);
  RadioSettings::Request manual;
  manual.regionCode = 3;
  manual.usePreset = false;
  manual.channel = 0;
  manual.bandwidth = 125;
  manual.spreadingFactor = 9;
  manual.codingRate = 5;
  LoRaPresets::Profile manualProfile{};
  USChannelSelection manualSelection{};
  assert(RadioSettings::resolve(manual, manualProfile, manualSelection, "Channel", "Custom"));
  assert(manualSelection.regionCode == 3); // manual mode never changes the selected region
  group("EU868/EU866/EU_N_868 sibling preset normalization is exact; manual settings retain region");

  RadioSettings::Request legacyUnset;
  legacyUnset.regionCode = 0;
  LoRaPresets::Profile unsetProfile{};
  USChannelSelection unsetSelection{};
  assert(!RadioSettings::resolve(legacyUnset, unsetProfile, unsetSelection,
                                 "LongFast", "LongFast"));
  unsetProfile = {11, 22, 3, 4};
  unsetSelection = {55, 66, 77, 88};
  const auto legacyProfileBefore = unsetProfile;
  const auto legacySelectionBefore = unsetSelection;
  assert(!RadioSettings::resolve(legacyUnset, unsetProfile, unsetSelection, "LongFast"));
  assert(!memcmp(&unsetProfile, &legacyProfileBefore, sizeof(unsetProfile)) &&
         same(unsetSelection, legacySelectionBefore));
  RadioSettings::Request nonUsLegacy;
  nonUsLegacy.regionCode = 2;
  assert(!RadioSettings::resolve(nonUsLegacy, unsetProfile, unsetSelection, "LongFast"));
  assert(!memcmp(&unsetProfile, &legacyProfileBefore, sizeof(unsetProfile)) &&
         same(unsetSelection, legacySelectionBefore));
  assert(RadioSettings::validateUnprovisioned(legacyUnset, unsetProfile,
                                              unsetSelection, "LongFast"));
  assert(unsetSelection.regionCode == 0 && unsetProfile.bandwidthHz == 250000);
  assert(!RadioSettings::validateUnprovisioned(manual, unsetProfile,
                                                unsetSelection, "LongFast"));
  group("legacy UNSET parameters may be structurally checked while selection remains unprovisioned");

  // A valid code must be checked before ManualLoRa's narrower enum helpers.
  RadioSettings::Request malformed;
  malformed.usePreset = false;
  malformed.bandwidth = 65544; // would narrow to the accepted 8 kHz enum
  LoRaPresets::Profile profileSentinel{111, 222, 33, 44};
  USChannelSelection selectionSentinel{55, 66, 77, 88};
  const auto profileBefore = profileSentinel;
  const auto selectionBefore = selectionSentinel;
  assert(!RadioSettings::resolve(malformed, profileSentinel, selectionSentinel, "Name", "Custom"));
  malformed.bandwidth = 125;
  malformed.spreadingFactor = 257;
  assert(!RadioSettings::resolve(malformed, profileSentinel, selectionSentinel, "Name", "Custom"));
  malformed.spreadingFactor = 9;
  malformed.codingRate = 260;
  assert(!RadioSettings::resolve(malformed, profileSentinel, selectionSentinel, "Name", "Custom"));
  assert(!memcmp(&profileSentinel, &profileBefore, sizeof(profileSentinel)));
  assert(same(selectionSentinel, selectionBefore));
  malformed.codingRate = 5;
  malformed.preset = UINT32_MAX;
  assert(RadioSettings::resolve(malformed, profileSentinel, selectionSentinel, "Name", "Custom"));
  assert(profileSentinel.bandwidthHz == 125000 && selectionSentinel.regionCode == 1);
  group("manual bandwidth, SF and coding-rate raw widths are rejected before narrowing");

  // Overrides use the exact float ordering, then nearest-Hz conversion; offset
  // is applied after the chosen center. Out-of-band and NaN inputs are atomic.
  const Region *us = mt::radio::RegionPolicy::lookup(1);
  assert(us);
  assert(stockOracle(*us, 125.0f, 0, "Name", "LongFast", 915.125f, 0.125f, want));
  assert(USChannel::lookupStock(*us, 125.0f, UINT32_MAX, "Name", "LongFast",
                                915.125f, 0.125f, got));
  assert(same(got, want) && got.slot == UINT16_MAX && got.frequencyHz == 915250000u);
  got = {9, 10, 11, 12};
  const auto overrideBefore = got;
  assert(!USChannel::lookupStock(*us, 125.0f, 1, "Name", "LongFast",
                                928.0f, 1.0f, got));
  assert(same(got, overrideBefore));
  assert(!USChannel::lookupStock(*us, 125.0f, 209, "Name", "LongFast", 0, 0, got));
  assert(same(got, overrideBefore));
  assert(!USChannel::lookupStock(*us, NAN, 1, "Name", "LongFast", 0, 0, got));
  assert(same(got, overrideBefore));
  assert(!USChannel::lookupStock(*us, 125.0f, 1, "Name", "LongFast", 0, INFINITY, got));
  assert(same(got, overrideBefore));
  assert(!USChannel::lookupStock(presetHashRegion, 250.0f, 0, "Name", nullptr, 0, 0, got));
  assert(same(got, overrideBefore));
  group("frequency override, offset, region edges, malformed floats and refusal atomicity pass");

  RadioSettings::Request power;
  assert(RadioSettings::effectivePower(power, true) == -9);
  assert(RadioSettings::effectivePower(power, false) == 2);
  power.powerSpecified = true;
  power.power = 30;
  assert(RadioSettings::effectivePower(power, true) == 22);
  power.regionCode = 2; // stock EU_433 cap is 10 dBm
  assert(RadioSettings::effectivePower(power, true) == 10);
  group("US board defaults are preserved and regional power limits cap SX1262 output");
}
