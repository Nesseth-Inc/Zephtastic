// SPDX-License-Identifier: GPL-3.0-only
// The runner inserts the exact pinned stock getRegionPresetMap() body from
// zephcore/bench/upstream-differential/source/RadioInterface.cpp.
#include "RegionCatalogue.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {
namespace RegionPolicy = mt::radio::RegionPolicy;
using mt::radio::RegionCatalogue::encode;
using mt::radio::RegionPolicy::Region;

const meshtastic_Config_LoRaConfig_ModemPreset kPresetEnd =
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(0xff);
const meshtastic_Config_LoRaConfig_ModemPreset kStandardPresets[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(0),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(1),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(3),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(4),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(5),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(6),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(7),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(8),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(9),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(16), kPresetEnd};
const meshtastic_Config_LoRaConfig_ModemPreset kEu868Presets[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(0),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(1),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(3),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(4),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(5),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(6),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(7), kPresetEnd};
const meshtastic_Config_LoRaConfig_ModemPreset kUnsetPresets[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(0), kPresetEnd};
const meshtastic_Config_LoRaConfig_ModemPreset kLitePresets[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(10),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(11), kPresetEnd};
const meshtastic_Config_LoRaConfig_ModemPreset kNarrowPresets[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(12),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(13), kPresetEnd};
const meshtastic_Config_LoRaConfig_ModemPreset kTinyPresets[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(14),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(15), kPresetEnd};

struct StockRegionProfile {
  const meshtastic_Config_LoRaConfig_ModemPreset *presets;
  float spacing;
  float padding;
  bool audioPermitted;
  bool licensedOnly;
  int8_t textThrottle;
  int8_t positionThrottle;
  int8_t telemetryThrottle;
};
struct StockRegionInfo {
  meshtastic_Config_LoRaConfig_RegionCode code;
  float freqStart;
  float freqEnd;
  float dutyCycle;
  uint8_t powerLimit;
  bool freqSwitching;
  bool wideLora;
  const StockRegionProfile *profile;
  meshtastic_Config_LoRaConfig_ModemPreset defaultPreset;
  int16_t overrideSlot;
  const char *name;
  meshtastic_Config_LoRaConfig_ModemPreset getDefaultPreset() const { return defaultPreset; }
};

const StockRegionProfile kStockStandard{kStandardPresets, 0, 0, true, false, 0, 1, 1};
const StockRegionProfile kStockEu868{kEu868Presets, 0, 0, false, false, 0, 1, 1};
const StockRegionProfile kStockUnset{kUnsetPresets, 0, 0, true, false, 0, 1, 1};
const StockRegionProfile kStockLite{kLitePresets, 0.4f, 0.0375f, false, false, 0, 10, 10};
const StockRegionProfile kStockNarrow{kNarrowPresets, 0, 0.0104f, true, false, 0, 1, 1};
const StockRegionProfile kStockHam20{kTinyPresets, 0, 0.0022f, false, true, 0, 2, 2};
const StockRegionProfile kStockHam100{kNarrowPresets, 0, 0.01875f, false, true, 0, 1, 1};

const meshtastic_Config_LoRaConfig_ModemPreset PRESETS_EU_SUPERSET[] = {
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(0),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(1),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(3),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(4),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(5),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(6),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(7),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(10),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(11),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(12),
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(13), kPresetEnd};

StockRegionInfo gStockRegions[RegionPolicy::kRegionCount]{};
const StockRegionInfo *const regions = gStockRegions;

const StockRegionProfile *stockProfile(const RegionPolicy::RegionProfile *profile) {
  if (profile == RegionPolicy::lookup(1)->profile) return &kStockStandard;
  if (profile == RegionPolicy::lookup(3)->profile) return &kStockEu868;
  if (profile == RegionPolicy::lookup(0)->profile) return &kStockUnset;
  if (profile == RegionPolicy::lookup(29)->profile) return &kStockLite;
  if (profile == RegionPolicy::lookup(32)->profile) return &kStockNarrow;
  if (profile == RegionPolicy::lookup(27)->profile) return &kStockHam20;
  if (profile == RegionPolicy::lookup(37)->profile) return &kStockHam100;
  return nullptr;
}

void prepareStockRegionRows() {
  assert(RegionPolicy::kRegionCount == 35);
  assert(RegionPolicy::regionAt(RegionPolicy::kRegionCount) == nullptr);
  for (size_t i = 0; i < RegionPolicy::kRegionCount; ++i) {
    const Region *r = RegionPolicy::regionAt(i);
    assert(r && r->profile);
    auto &dst = gStockRegions[i];
    dst.code = static_cast<meshtastic_Config_LoRaConfig_RegionCode>(r->code);
    dst.freqStart = r->freqStartMHz;
    dst.freqEnd = r->freqEndMHz;
    dst.dutyCycle = r->dutyCyclePercent;
    dst.powerLimit = r->powerLimitDbm;
    dst.freqSwitching = r->frequencySwitching;
    dst.wideLora = r->wideLora;
    dst.profile = stockProfile(r->profile);
    dst.defaultPreset = static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(r->defaultPreset);
    dst.overrideSlot = r->overrideSlot;
    dst.name = r->name;
    assert(dst.profile);
  }
}

[[maybe_unused]] const StockRegionInfo *getRegion(meshtastic_Config_LoRaConfig_RegionCode code) {
  for (const auto &region : gStockRegions) if (region.code == code) return &region;
  return nullptr;
}

bool isSwappableEuRegion(meshtastic_Config_LoRaConfig_RegionCode code) {
  return code == static_cast<meshtastic_Config_LoRaConfig_RegionCode>(3) ||
         code == static_cast<meshtastic_Config_LoRaConfig_RegionCode>(29) ||
         code == static_cast<meshtastic_Config_LoRaConfig_RegionCode>(32);
}
} // namespace

using RegionProfile = StockRegionProfile;
using RegionInfo = StockRegionInfo;
const meshtastic_Config_LoRaConfig_ModemPreset MODEM_PRESET_END = kPresetEnd;
#define LOG_ERROR(...) ((void)0)
// This include is generated by the runner from the exact pinned upstream
// function body. Keep the upstream implementation, not a copied approximation.
#include "stock_catalogue_producer.inc"
#undef LOG_ERROR

namespace {
bool sameMap(const meshtastic_LoRaRegionPresetMap &a,
             const meshtastic_LoRaRegionPresetMap &b) {
  if (a.groups_count != b.groups_count || a.region_groups_count != b.region_groups_count) return false;
  for (pb_size_t i = 0; i < a.groups_count; ++i) {
    const auto &x = a.groups[i];
    const auto &y = b.groups[i];
    if (x.presets_count != y.presets_count || x.default_preset != y.default_preset ||
        x.licensed_only != y.licensed_only) return false;
    for (pb_size_t j = 0; j < x.presets_count; ++j) if (x.presets[j] != y.presets[j]) return false;
  }
  for (pb_size_t i = 0; i < a.region_groups_count; ++i) {
    if (a.region_groups[i].region != b.region_groups[i].region ||
        a.region_groups[i].group_index != b.region_groups[i].group_index) return false;
  }
  return true;
}

void assertLiteralStockMap(const meshtastic_LoRaRegionPresetMap &map) {
  constexpr uint32_t codes[] = {1, 2, 3, 29, 32, 4, 5, 6, 22, 9, 7, 8, 10, 11, 12, 14, 16,
      17, 18, 19, 20, 21, 23, 24, 25, 26, 27, 28, 33, 37, 34, 35, 36, 13};
  constexpr uint8_t groups[] = {0, 0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 5, 5, 5, 5, 0};
  assert(map.groups_count == 6 && map.region_groups_count == 34);
  constexpr uint8_t standard[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 16};
  constexpr uint8_t eu[] = {0, 1, 3, 4, 5, 6, 7, 10, 11, 12, 13};
  constexpr uint8_t ham20[] = {14, 15};
  constexpr uint8_t ham100[] = {12, 13};
  const uint8_t *presets[] = {standard, eu, eu, eu, ham20, ham100};
  const uint8_t counts[] = {10, 11, 11, 11, 2, 2};
  const uint8_t defaults[] = {0, 0, 10, 13, 14, 13};
  const bool licensed[] = {false, false, false, false, true, true};
  for (pb_size_t g = 0; g < map.groups_count; ++g) {
    const auto &group = map.groups[g];
    assert(group.presets_count == counts[g]);
    assert(static_cast<uint32_t>(group.default_preset) == defaults[g]);
    assert(group.licensed_only == licensed[g]);
    for (pb_size_t p = 0; p < group.presets_count; ++p)
      assert(static_cast<uint32_t>(group.presets[p]) == presets[g][p]);
  }
  for (pb_size_t i = 0; i < map.region_groups_count; ++i) {
    assert(static_cast<uint32_t>(map.region_groups[i].region) == codes[i]);
    assert(map.region_groups[i].group_index == groups[i]);
  }
}

void assertDecodedEqual(const meshtastic_FromRadio &a, const meshtastic_FromRadio &b) {
  assert(a.which_payload_variant == meshtastic_FromRadio_region_presets_tag);
  assert(b.which_payload_variant == meshtastic_FromRadio_region_presets_tag);
  assert(a.id == b.id);
  assert(sameMap(a.region_presets, b.region_presets));
}

void testStockDifferentialAndWireRoundTrip() {
  prepareStockRegionRows();
  meshtastic_LoRaRegionPresetMap actual{};
  meshtastic_LoRaRegionPresetMap stock{};
  assert(encode(actual));
  getRegionPresetMap(stock);
  assert(sameMap(actual, stock));
  assertLiteralStockMap(actual);

  meshtastic_FromRadio frame{};
  frame.id = 3; // MyInfo=1, metadata=2, catalogue=3 in this Companion handshake.
  frame.which_payload_variant = meshtastic_FromRadio_region_presets_tag;
  frame.region_presets = actual;
  uint8_t wire[512]{};
  auto stream = pb_ostream_from_buffer(wire, sizeof(wire));
  assert(pb_encode(&stream, meshtastic_FromRadio_fields, &frame));
  assert(stream.bytes_written <= 510 && stream.bytes_written < sizeof(wire));

  meshtastic_FromRadio decoded{};
  auto input = pb_istream_from_buffer(wire, stream.bytes_written);
  assert(pb_decode(&input, meshtastic_FromRadio_fields, &decoded));
  assert(input.bytes_left == 0);
  assertDecodedEqual(frame, decoded);

  uint8_t reencoded[512]{};
  auto output = pb_ostream_from_buffer(reencoded, sizeof(reencoded));
  assert(pb_encode(&output, meshtastic_FromRadio_fields, &decoded));
  assert(output.bytes_written == stream.bytes_written);
  assert(memcmp(wire, reencoded, stream.bytes_written) == 0);

  printf("WIREHEX:");
  for (size_t i = 0; i < stream.bytes_written; ++i) printf("%02x", wire[i]);
  puts("");
}
} // namespace

int main() {
  testStockDifferentialAndWireRoundTrip();
  puts("PASS region catalogue: exact stock producer, 6 groups/34 rows, enum/license flags, bounded wire round-trip");
}
