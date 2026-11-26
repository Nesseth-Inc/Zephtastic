// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "LoRaPresets.h"
#include "ManualLoRa.h"
#include "RegionPolicy.h"
#include "USChannel.h"

#include <stdint.h>

namespace mt {

// Selection validity is a software-path check for the pinned SX126x profile.
// It does not establish board RF compatibility, legal permission, or operation.
struct RadioSettings {
  struct Request {
    bool usePreset = true;
    bool powerSpecified = false;
    uint32_t regionCode = 1; // stock RegionCode_US; validated before any cast
    uint32_t preset = 0, channel = 20, bandwidth = 0, spreadingFactor = 0,
             codingRate = 0;
    int32_t power = 0;
    float overrideMHz = 0, offsetMHz = 0;
  };

  static bool presetSupported(unsigned preset) {
    static const uint8_t kUsPresets[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 16};
    for (uint8_t value : kUsPresets)
      if (value == preset) return true;
    return false;
  }

  // Validate the raw protobuf-compatible region code and normalize the three
  // stock-swappable EU 868 siblings when a preset belongs to another sibling.
  // Returns the selected code separately so callers can persist it atomically.
  static bool normalizeRegionPreset(uint32_t requestedRegionCode, bool usePreset,
                                    uint32_t preset, uint32_t &selectedRegionCode,
                                    bool explicitlyTxOff = false) {
    using namespace radio::RegionPolicy;
    if (usePreset && preset > 16) return false;
    const Region *region = lookup(requestedRegionCode);
    if (!region || !tunable(*region, explicitlyTxOff)) return false;

    if (usePreset && !supportsPreset(*region->profile, preset)) {
      if (requestedRegionCode != 3 && requestedRegionCode != 29 &&
          requestedRegionCode != 32)
        return false;
      constexpr uint32_t kEu868Siblings[] = {3, 29, 32};
      const Region *sibling = nullptr;
      for (uint32_t code : kEu868Siblings) {
        if (code == requestedRegionCode) continue;
        const Region *candidate = lookup(code);
        if (candidate && supportsPreset(*candidate->profile, preset)) {
          if (sibling) return false; // require one unambiguous owning sibling
          sibling = candidate;
        }
      }
      if (!sibling || !softwareUsableOnSx126x(*sibling, false, false)) return false;
      region = sibling;
    }
    selectedRegionCode = region->code;
    return true;
  }

  static bool resolve(const Request &request, LoRaPresets::Profile &profile,
                      USChannelSelection &selection, const char *channelName,
                      const char *presetDisplayName, bool explicitlyTxOff = false) {
    if ((request.usePreset && request.preset > 16) ||
        (request.powerSpecified && (request.power < -128 || request.power > 127)))
      return false;

    uint32_t selectedRegionCode = 0;
    if (!normalizeRegionPreset(request.regionCode, request.usePreset,
                               request.preset, selectedRegionCode, explicitlyTxOff))
      return false;
    const auto *region = radio::RegionPolicy::lookup(selectedRegionCode);
    if (!region) return false;

    LoRaPresets::Profile candidateProfile{};
    USChannelSelection candidateSelection{};
    float bandwidthKHz = 0.0f;
    if (request.usePreset) {
      if (!LoRaPresets::lookup(request.preset, candidateProfile)) return false;
      if (request.codingRate >= 5 && request.codingRate <= 8 &&
          request.codingRate > candidateProfile.codingDenominator)
        candidateProfile.codingDenominator = static_cast<uint8_t>(request.codingRate);
      bandwidthKHz = static_cast<float>(candidateProfile.bandwidthHz) / 1000.0f;
    } else {
      // ManualLoRa maps a few stock bandwidth enum values through uint16_t and
      // its SF/CR helpers narrow to uint8_t. Refuse wide raw values first.
      if (request.bandwidth > UINT16_MAX || request.spreadingFactor > UINT8_MAX ||
          request.codingRate > UINT8_MAX ||
          !ManualLoRa::resolve(request.bandwidth, request.spreadingFactor,
                               request.codingRate, candidateProfile))
        return false;
      bandwidthKHz = ManualLoRa::bandwidthKHz(request.bandwidth);
    }

    if (!USChannel::lookupStock(*region, bandwidthKHz, request.channel,
                                channelName, presetDisplayName,
                                request.overrideMHz, request.offsetMHz,
                                candidateSelection))
      return false;
    profile = candidateProfile;
    selection = candidateSelection;
    return true;
  }

  // Four-argument compatibility resolver is deliberately US-only. It never
  // guesses a regional preset name from the channel name.
  static bool resolve(const Request &request, LoRaPresets::Profile &profile,
                      USChannelSelection &selection, const char *channelName) {
    if (request.regionCode != 1) return false;
    Request usRequest = request;
    LoRaPresets::Profile candidateProfile{};
    USChannelSelection candidateSelection{};
    if (resolve(usRequest, candidateProfile, candidateSelection, channelName, nullptr)) {
      profile = candidateProfile;
      selection = candidateSelection;
      return true;
    }
    // Preserve the old US-only resolver's behavior for channel values that
    // Config::setRadio will clamp to the computed default hash slot.
    if (usRequest.channel == 0 || usRequest.overrideMHz != 0.0f) return false;
    usRequest.channel = 0;
    if (!resolve(usRequest, candidateProfile, candidateSelection, channelName, nullptr))
      return false;
    profile = candidateProfile;
    selection = candidateSelection;
    return true;
  }

  // UNSET records have no active region. This compatibility validator checks
  // their legacy US-shaped parameters for structural/readback consistency
  // only; the returned selection remains explicitly unprovisioned (region 0).
  // Callers must not use this result to configure a radio or permit TX.
  static bool validateUnprovisioned(const Request &request,
                                    LoRaPresets::Profile &profile,
                                    USChannelSelection &selection,
                                    const char *channelName) {
    if (request.regionCode != 0) return false;
    Request legacy = request;
    legacy.regionCode = 1;
    LoRaPresets::Profile candidateProfile{};
    USChannelSelection candidateSelection{};
    if (!resolve(legacy, candidateProfile, candidateSelection, channelName)) return false;
    candidateSelection.regionCode = 0;
    profile = candidateProfile;
    selection = candidateSelection;
    return true;
  }

  static int effectivePower(const Request &request, bool t096) {
    const int boardDefault = t096 ? -9 : 2;
    const auto *region = radio::RegionPolicy::lookup(request.regionCode);
    if (!region || !radio::RegionPolicy::softwareUsableOnSx126x(*region, false, false))
      return boardDefault;
    const int selected = request.powerSpecified
        ? ManualLoRa::regionalPower(request.power, region->powerLimitDbm, false)
        : boardDefault;
    // Both supported board profiles bind SX1262. Preserve their legacy
    // minimum and cap at the selected region's stock limit and SX1262 maximum.
    return selected < -9 ? -9 : selected > 22 ? 22 : selected;
  }

  // Original US-only helper retained for existing call sites and migration
  // tests. Its allowlist is the US standard preset profile.
  static bool resolve(unsigned preset, uint32_t channel,
                      LoRaPresets::Profile &profile,
                      USChannelSelection &selection,
                      const char *name = "LongFast") {
    if (!presetSupported(preset)) return false;
    LoRaPresets::Profile candidateProfile{};
    USChannelSelection candidateSelection{};
    if (!LoRaPresets::lookup(preset, candidateProfile) ||
        !USChannel::lookup(candidateProfile.bandwidthHz, channel, name,
                           candidateSelection))
      return false;
    profile = candidateProfile;
    selection = candidateSelection;
    return true;
  }
};

} // namespace mt
