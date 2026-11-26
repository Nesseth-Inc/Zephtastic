// SPDX-License-Identifier: GPL-3.0-only
// Literal oracle copied from pinned meshtastic/firmware@8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c
// src/mesh/RadioInterface.cpp (SHA256 0de8434b14c3c1deb383b4510b96a9d0d750a797e9140f9cfb08147e4a749506)
// and src/mesh/MeshRadio.h (SHA256 06666c3d7ed9744ad71ec04ac50409e82d2b7fb3339377a8647140c35ff19698).
#include "RegionPolicy.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
using namespace mt::radio::RegionPolicy;

struct ExpectedProfile {
  const char *name;
  uint8_t presets[10];
  uint8_t presetCount;
  float spacingMHz;
  float paddingMHz;
  bool audioPermitted;
  bool licensedOnly;
  int8_t textThrottle;
  int8_t positionThrottle;
  int8_t telemetryThrottle;
};

constexpr ExpectedProfile kProfiles[] = {
    {"PROFILE_STD", {0, 1, 3, 4, 5, 6, 7, 8, 9, 16}, 10, 0.0f, 0.0f, true, false, 0, 1, 1},
    {"PROFILE_EU868", {0, 1, 3, 4, 5, 6, 7}, 7, 0.0f, 0.0f, false, false, 0, 1, 1},
    {"PROFILE_UNDEF", {0}, 1, 0.0f, 0.0f, true, false, 0, 1, 1},
    {"PROFILE_LITE", {10, 11}, 2, 0.4f, 0.0375f, false, false, 0, 10, 10},
    {"PROFILE_NARROW", {12, 13}, 2, 0.0f, 0.0104f, true, false, 0, 1, 1},
    {"PROFILE_HAM_20KHZ", {14, 15}, 2, 0.0f, 0.0022f, false, true, 0, 2, 2},
    {"PROFILE_HAM_100KHZ", {12, 13}, 2, 0.0f, 0.01875f, false, true, 0, 1, 1},
};

enum ProfileIndex : uint8_t { Std, Eu868, Undef, Lite, Narrow, Ham20, Ham100 };
struct ExpectedRegion {
  uint32_t code;
  const char *name;
  float start;
  float end;
  float duty;
  uint8_t power;
  bool frequencySwitching;
  bool wideLora;
  ProfileIndex profile;
  uint8_t defaultPreset;
  int16_t overrideSlot;
};

// This is a test oracle, not production data inclusion. Region enum numbers and
// each RDEF tuple are literals from the pinned source named above.
constexpr ExpectedRegion kRegions[] = {
    {1, "US", 902.0f, 928.0f, 100.0f, 30, false, false, Std, 0, 0},
    {2, "EU_433", 433.0f, 434.0f, 10.0f, 10, false, false, Std, 0, 0},
    {3, "EU_868", 869.4f, 869.65f, 10.0f, 27, false, false, Eu868, 0, 0},
    {29, "EU_866", 865.6f, 867.6f, 2.5f, 27, false, false, Lite, 10, 0},
    {32, "EU_N_868", 869.4f, 869.65f, 10.0f, 27, false, false, Narrow, 13, 1},
    {4, "CN", 470.0f, 510.0f, 100.0f, 19, false, false, Std, 0, 0},
    {5, "JP", 920.5f, 923.5f, 100.0f, 13, false, false, Std, 0, 0},
    {6, "ANZ", 915.0f, 928.0f, 100.0f, 30, false, false, Std, 0, 0},
    {22, "ANZ_433", 433.05f, 434.79f, 100.0f, 14, false, false, Std, 0, 0},
    {9, "RU", 868.7f, 869.2f, 100.0f, 20, false, false, Std, 0, 0},
    {7, "KR", 920.0f, 923.0f, 100.0f, 23, false, false, Std, 0, 0},
    {8, "TW", 920.0f, 925.0f, 100.0f, 27, false, false, Std, 0, 0},
    {10, "IN", 865.0f, 867.0f, 100.0f, 30, false, false, Std, 0, 0},
    {11, "NZ_865", 864.0f, 868.0f, 100.0f, 36, false, false, Std, 0, 0},
    {12, "TH", 920.0f, 925.0f, 10.0f, 27, false, false, Std, 0, 0},
    {14, "UA_433", 433.0f, 434.7f, 10.0f, 10, false, false, Std, 0, 0},
    {16, "MY_433", 433.0f, 435.0f, 100.0f, 20, false, false, Std, 0, 0},
    {17, "MY_919", 919.0f, 924.0f, 100.0f, 27, true, false, Std, 0, 0},
    {18, "SG_923", 917.0f, 925.0f, 100.0f, 20, false, false, Std, 0, 0},
    {19, "PH_433", 433.0f, 434.7f, 100.0f, 10, false, false, Std, 0, 0},
    {20, "PH_868", 868.0f, 869.4f, 100.0f, 14, false, false, Std, 0, 0},
    {21, "PH_915", 915.0f, 918.0f, 100.0f, 24, false, false, Std, 0, 0},
    {23, "KZ_433", 433.075f, 434.775f, 100.0f, 10, false, false, Std, 0, 0},
    {24, "KZ_863", 863.0f, 868.0f, 100.0f, 30, false, false, Std, 0, 0},
    {25, "NP_865", 865.0f, 868.0f, 100.0f, 30, false, false, Std, 0, 0},
    {26, "BR_902", 902.0f, 907.5f, 100.0f, 30, false, false, Std, 0, 0},
    {27, "ITU1_2M", 144.0f, 146.0f, 100.0f, 30, false, false, Ham20, 14, 26},
    {28, "ITU2_2M", 144.0f, 148.0f, 100.0f, 30, false, false, Ham20, 14, 51},
    {33, "ITU3_2M", 144.0f, 148.0f, 100.0f, 30, false, false, Ham20, 14, 33},
    {37, "ITU2_125CM", 220.0f, 225.0f, 100.0f, 30, false, false, Ham100, 13, 37},
    {34, "ITU1_70CM", 430.0f, 440.0f, 100.0f, 30, false, false, Ham100, 13, 37},
    {35, "ITU2_70CM", 420.0f, 450.0f, 100.0f, 30, false, false, Ham100, 13, 137},
    {36, "ITU3_70CM", 430.0f, 450.0f, 100.0f, 30, false, false, Ham100, 13, 37},
    {13, "LORA_24", 2400.0f, 2483.5f, 100.0f, 10, false, true, Std, 0, 0},
    {0, "UNSET", 902.0f, 928.0f, 100.0f, 30, false, false, Undef, 0, 0},
};

bool close(float left, float right) { return fabsf(left - right) < 0.00001f; }

void testAllProfiles() {
  const Region *us = lookup(1);
  assert(us != nullptr);
  const RegionProfile *actual[] = {us->profile, lookup(3)->profile, lookup(0)->profile,
                                   lookup(29)->profile, lookup(32)->profile, lookup(27)->profile,
                                   lookup(37)->profile};
  for (size_t i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); ++i) {
    const RegionProfile &got = *actual[i];
    const ExpectedProfile &expected = kProfiles[i];
    assert(got.presetCount == expected.presetCount);
    assert(close(got.spacingMHz, expected.spacingMHz));
    assert(close(got.paddingMHz, expected.paddingMHz));
    assert(got.audioPermitted == expected.audioPermitted);
    assert(got.licensedOnly == expected.licensedOnly);
    assert(got.textThrottle == expected.textThrottle);
    assert(got.positionThrottle == expected.positionThrottle);
    assert(got.telemetryThrottle == expected.telemetryThrottle);
    for (uint8_t j = 0; j < got.presetCount; ++j) {
      assert(got.presets[j] == expected.presets[j]);
      assert(supportsPreset(got, expected.presets[j]));
    }
    for (uint32_t preset = 0; preset <= 16; ++preset) {
      bool expectedSupport = false;
      for (uint8_t j = 0; j < expected.presetCount; ++j) {
        if (expected.presets[j] == preset) expectedSupport = true;
      }
      assert(supportsPreset(got, preset) == expectedSupport);
    }
    assert(!supportsPreset(got, UINT32_MAX));
  }
  assert(supportsPreset(*lookup(1)->profile, 16));
  assert(!supportsPreset(*lookup(1)->profile, 2));
  assert(supportsPreset(*lookup(29)->profile, 10));
  assert(!supportsPreset(*lookup(29)->profile, 13));
}

void testAllRows() {
  assert(kRegionCount == sizeof(kRegions) / sizeof(kRegions[0]));
  assert(kRegionCount == 35);
  for (const ExpectedRegion &expected : kRegions) {
    const Region *got = lookup(expected.code);
    assert(got != nullptr);
    assert(got->code == expected.code);
    assert(strcmp(got->name, expected.name) == 0);
    assert(close(got->freqStartMHz, expected.start));
    assert(close(got->freqEndMHz, expected.end));
    assert(close(got->dutyCyclePercent, expected.duty));
    assert(got->powerLimitDbm == expected.power);
    assert(got->frequencySwitching == expected.frequencySwitching);
    assert(got->wideLora == expected.wideLora);
    assert(got->defaultPreset == expected.defaultPreset);
    assert(got->overrideSlot == expected.overrideSlot);
    const RegionProfile *expectedProfile = nullptr;
    switch (expected.profile) {
      case Std: expectedProfile = lookup(1)->profile; break;
      case Eu868: expectedProfile = lookup(3)->profile; break;
      case Undef: expectedProfile = lookup(0)->profile; break;
      case Lite: expectedProfile = lookup(29)->profile; break;
      case Narrow: expectedProfile = lookup(32)->profile; break;
      case Ham20: expectedProfile = lookup(27)->profile; break;
      case Ham100: expectedProfile = lookup(37)->profile; break;
    }
    assert(got->profile == expectedProfile);
  }
}

void testUnknownCodesUnsetAndUnsupportedRequirements() {
  assert(lookup(15) == nullptr);
  assert(lookup(30) == nullptr);
  assert(lookup(31) == nullptr);
  assert(lookup(UINT32_MAX) == nullptr);
  assert(lookup(38) == nullptr);
  assert(lookup(0) != nullptr);
  assert(!provisioned(*lookup(0)));
  assert(provisioned(*lookup(1)));

  assert(!softwareUsableOnSx126x(*lookup(13), false, false));
  assert(!softwareUsableOnSx126x(*lookup(27), false, false));
  assert(softwareUsableOnSx126x(*lookup(27), true, false));
  assert(!softwareUsableOnSx126x(*lookup(17), false, false));
  assert(softwareUsableOnSx126x(*lookup(17), false, true));
  assert(softwareUsableOnSx126x(*lookup(1), false, false));
  assert(!softwareUsableOnSx126x(*lookup(0), true, true));

  assert(close(effectiveDutyPercent(*lookup(29), DeviceRole::Mobile), 2.5f));
  assert(close(effectiveDutyPercent(*lookup(29), DeviceRole::FixedRouter), 10.0f));
  assert(close(effectiveDutyPercent(*lookup(1), DeviceRole::FixedRouter), 100.0f));
}
} // namespace

int main() {
  testAllProfiles();
  testAllRows();
  testUnknownCodesUnsetAndUnsupportedRequirements();
  puts("PASS stock regional table: 35 rows, 7 complete profiles, unknown IDs, exclusions, duty-role rule");
}
