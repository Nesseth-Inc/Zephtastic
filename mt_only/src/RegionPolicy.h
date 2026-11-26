// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

// Compact, immutable projection of the pinned stock RegionInfo/RegionProfile
// tables. Values are stock protobuf enum numbers so unknown caller values are
// checked before any narrowing cast. This policy table is not RF permission or
// proof that a particular board's front end supports every listed frequency.
namespace mt::radio::RegionPolicy {

struct RegionProfile {
  const uint8_t *presets;
  uint8_t presetCount;
  float spacingMHz;
  float paddingMHz;
  bool audioPermitted;
  bool licensedOnly;
  int8_t textThrottle;
  int8_t positionThrottle;
  int8_t telemetryThrottle;
};

struct Region {
  const char *name;
  const RegionProfile *profile;
  float freqStartMHz;
  float freqEndMHz;
  float dutyCyclePercent;
  int16_t overrideSlot;
  uint8_t code;
  uint8_t powerLimitDbm;
  uint8_t defaultPreset;
  bool frequencySwitching;
  bool wideLora;

  constexpr Region(uint8_t code, const char *name, float start, float end, float duty,
                   uint8_t power, bool switching, bool wide, const RegionProfile *profile,
                   uint8_t preset, int16_t slot)
      : name(name), profile(profile), freqStartMHz(start), freqEndMHz(end), dutyCyclePercent(duty),
        overrideSlot(slot), code(code), powerLimitDbm(power), defaultPreset(preset),
        frequencySwitching(switching), wideLora(wide) {}
};

enum class DeviceRole : uint8_t { Mobile, FixedRouter };

namespace detail {
constexpr uint8_t kPresetsStandard[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 16};
constexpr uint8_t kPresetsEu868[] = {0, 1, 3, 4, 5, 6, 7};
constexpr uint8_t kPresetsUnset[] = {0};
constexpr uint8_t kPresetsLite[] = {10, 11};
constexpr uint8_t kPresetsNarrow[] = {12, 13};
constexpr uint8_t kPresetsTiny[] = {14, 15};

constexpr RegionProfile kProfileStandard{kPresetsStandard, 10, 0.0f, 0.0f, true, false, 0, 1, 1};
constexpr RegionProfile kProfileEu868{kPresetsEu868, 7, 0.0f, 0.0f, false, false, 0, 1, 1};
constexpr RegionProfile kProfileUnset{kPresetsUnset, 1, 0.0f, 0.0f, true, false, 0, 1, 1};
constexpr RegionProfile kProfileLite{kPresetsLite, 2, 0.4f, 0.0375f, false, false, 0, 10, 10};
constexpr RegionProfile kProfileNarrow{kPresetsNarrow, 2, 0.0f, 0.0104f, true, false, 0, 1, 1};
constexpr RegionProfile kProfileHam20{kPresetsTiny, 2, 0.0f, 0.0022f, false, true, 0, 2, 2};
constexpr RegionProfile kProfileHam100{kPresetsNarrow, 2, 0.0f, 0.01875f, false, true, 0, 1, 1};

constexpr Region kRegions[] = {
    {1, "US", 902.0f, 928.0f, 100.0f, 30, false, false, &kProfileStandard, 0, 0},
    {2, "EU_433", 433.0f, 434.0f, 10.0f, 10, false, false, &kProfileStandard, 0, 0},
    {3, "EU_868", 869.4f, 869.65f, 10.0f, 27, false, false, &kProfileEu868, 0, 0},
    {29, "EU_866", 865.6f, 867.6f, 2.5f, 27, false, false, &kProfileLite, 10, 0},
    {32, "EU_N_868", 869.4f, 869.65f, 10.0f, 27, false, false, &kProfileNarrow, 13, 1},
    {4, "CN", 470.0f, 510.0f, 100.0f, 19, false, false, &kProfileStandard, 0, 0},
    {5, "JP", 920.5f, 923.5f, 100.0f, 13, false, false, &kProfileStandard, 0, 0},
    {6, "ANZ", 915.0f, 928.0f, 100.0f, 30, false, false, &kProfileStandard, 0, 0},
    {22, "ANZ_433", 433.05f, 434.79f, 100.0f, 14, false, false, &kProfileStandard, 0, 0},
    {9, "RU", 868.7f, 869.2f, 100.0f, 20, false, false, &kProfileStandard, 0, 0},
    {7, "KR", 920.0f, 923.0f, 100.0f, 23, false, false, &kProfileStandard, 0, 0},
    {8, "TW", 920.0f, 925.0f, 100.0f, 27, false, false, &kProfileStandard, 0, 0},
    {10, "IN", 865.0f, 867.0f, 100.0f, 30, false, false, &kProfileStandard, 0, 0},
    {11, "NZ_865", 864.0f, 868.0f, 100.0f, 36, false, false, &kProfileStandard, 0, 0},
    {12, "TH", 920.0f, 925.0f, 10.0f, 27, false, false, &kProfileStandard, 0, 0},
    {14, "UA_433", 433.0f, 434.7f, 10.0f, 10, false, false, &kProfileStandard, 0, 0},
    {16, "MY_433", 433.0f, 435.0f, 100.0f, 20, false, false, &kProfileStandard, 0, 0},
    {17, "MY_919", 919.0f, 924.0f, 100.0f, 27, true, false, &kProfileStandard, 0, 0},
    {18, "SG_923", 917.0f, 925.0f, 100.0f, 20, false, false, &kProfileStandard, 0, 0},
    {19, "PH_433", 433.0f, 434.7f, 100.0f, 10, false, false, &kProfileStandard, 0, 0},
    {20, "PH_868", 868.0f, 869.4f, 100.0f, 14, false, false, &kProfileStandard, 0, 0},
    {21, "PH_915", 915.0f, 918.0f, 100.0f, 24, false, false, &kProfileStandard, 0, 0},
    {23, "KZ_433", 433.075f, 434.775f, 100.0f, 10, false, false, &kProfileStandard, 0, 0},
    {24, "KZ_863", 863.0f, 868.0f, 100.0f, 30, false, false, &kProfileStandard, 0, 0},
    {25, "NP_865", 865.0f, 868.0f, 100.0f, 30, false, false, &kProfileStandard, 0, 0},
    {26, "BR_902", 902.0f, 907.5f, 100.0f, 30, false, false, &kProfileStandard, 0, 0},
    {27, "ITU1_2M", 144.0f, 146.0f, 100.0f, 30, false, false, &kProfileHam20, 14, 26},
    {28, "ITU2_2M", 144.0f, 148.0f, 100.0f, 30, false, false, &kProfileHam20, 14, 51},
    {33, "ITU3_2M", 144.0f, 148.0f, 100.0f, 30, false, false, &kProfileHam20, 14, 33},
    {37, "ITU2_125CM", 220.0f, 225.0f, 100.0f, 30, false, false, &kProfileHam100, 13, 37},
    {34, "ITU1_70CM", 430.0f, 440.0f, 100.0f, 30, false, false, &kProfileHam100, 13, 37},
    {35, "ITU2_70CM", 420.0f, 450.0f, 100.0f, 30, false, false, &kProfileHam100, 13, 137},
    {36, "ITU3_70CM", 430.0f, 450.0f, 100.0f, 30, false, false, &kProfileHam100, 13, 37},
    {13, "LORA_24", 2400.0f, 2483.5f, 100.0f, 10, false, true, &kProfileStandard, 0, 0},
    {0, "UNSET", 902.0f, 928.0f, 100.0f, 30, false, false, &kProfileUnset, 0, 0},
};
} // namespace detail

constexpr size_t kRegionCount = sizeof(detail::kRegions) / sizeof(detail::kRegions[0]);

inline const Region *regionAt(size_t index) {
  return index < kRegionCount ? &detail::kRegions[index] : nullptr;
}

inline const Region *lookup(uint32_t regionCode) {
  for (size_t i = 0; i < kRegionCount; ++i) {
    const Region *region = regionAt(i);
    if (region && region->code == regionCode) return region;
  }
  return nullptr;
}

inline bool supportsPreset(const RegionProfile &profile, uint32_t preset) {
  for (uint8_t i = 0; i < profile.presetCount; ++i) {
    if (profile.presets[i] == preset) return true;
  }
  return false;
}

inline float effectiveDutyPercent(const Region &region, DeviceRole role) {
  if (region.code == 29) return role == DeviceRole::FixedRouter ? 10.0f : 2.5f;
  return region.dutyCyclePercent;
}

// Checks software-path exclusions only. It does not establish RF-front-end,
// antenna, band, jurisdiction, or operational suitability for a board.
inline bool softwareUsableOnSx126x(const Region &region,
                                   bool licensedStatePresentAndValid,
                                   bool frequencySwitchingSchedulerPresent) {
  if (region.code == 0 || region.wideLora) return false;
  if (region.profile->licensedOnly && !licensedStatePresentAndValid) return false;
  if (region.frequencySwitching && !frequencySwitchingSchedulerPresent) return false;
  return true;
}

// Stock MY_919 selects one fixed slot. The XIAO Wio radio covers this band,
// but no hopping scheduler exists. This exception is for receiving only.
constexpr bool fixedReceiveOnly(uint32_t code) {
#if defined(CONFIG_BOARD_XIAO_ESP32S3)
  return code == 17;
#else
  (void)code;
  return false;
#endif
}

inline bool tunable(const Region &region, bool explicitlyTxOff) {
  return (explicitlyTxOff && fixedReceiveOnly(region.code)) ||
         softwareUsableOnSx126x(region, false, false);
}

inline bool provisioned(const Region &region) { return region.code != 0; }

} // namespace mt::radio::RegionPolicy
