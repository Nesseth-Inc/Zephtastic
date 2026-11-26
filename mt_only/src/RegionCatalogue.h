// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "RegionPolicy.h"
#include "meshtastic/mesh.pb.h"

#include <stddef.h>
#include <stdint.h>

// Public Companion UI catalogue only. This map does not authorize a region or
// alter the independent runtime radio/configuration admission checks.
namespace mt::radio::RegionCatalogue {
namespace detail {

inline bool isEuPresetSwapRegion(uint32_t code) {
  return code == 3 || code == 29 || code == 32;
}

inline bool fail(meshtastic_LoRaRegionPresetMap &out) {
  out = {};
  return false;
}

inline bool appendPreset(meshtastic_LoRaPresetGroup &group,
                         size_t capacity, uint8_t preset) {
  for (pb_size_t i = 0; i < group.presets_count; ++i) {
    if (static_cast<uint32_t>(group.presets[i]) == preset) return true;
  }
  if (group.presets_count >= capacity) return false;
  group.presets[group.presets_count++] =
      static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(preset);
  return true;
}

inline bool appendProfilePresets(meshtastic_LoRaPresetGroup &group,
                                 size_t capacity,
                                 const RegionPolicy::RegionProfile *profile) {
  if (!profile || (profile->presetCount && !profile->presets)) return false;
  for (uint8_t i = 0; i < profile->presetCount; ++i) {
    if (!appendPreset(group, capacity, profile->presets[i])) return false;
  }
  return true;
}

inline const RegionPolicy::Region *regionForMappedCode(
    const meshtastic_LoRaRegionPresets &mapped) {
  return RegionPolicy::lookup(static_cast<uint32_t>(mapped.region));
}

inline int findProfileGroup(const meshtastic_LoRaRegionPresetMap &out,
                            const RegionPolicy::Region &region) {
  for (pb_size_t groupIndex = 0; groupIndex < out.groups_count; ++groupIndex) {
    if (static_cast<uint32_t>(out.groups[groupIndex].default_preset) !=
        region.defaultPreset)
      continue;
    for (pb_size_t i = 0; i < out.region_groups_count; ++i) {
      if (out.region_groups[i].group_index != groupIndex) continue;
      const auto *previous = regionForMappedCode(out.region_groups[i]);
      if (previous && previous->profile == region.profile) return groupIndex;
    }
  }
  return -1;
}

inline bool fillGroup(meshtastic_LoRaPresetGroup &group,
                      const RegionPolicy::Region &region,
                      size_t presetCapacity) {
  group = {};
  group.default_preset =
      static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(region.defaultPreset);
  group.licensed_only = region.profile->licensedOnly;
  if (isEuPresetSwapRegion(region.code)) {
    // Stock UI presents the trio's exact union because selecting another
    // sibling's preset requests an automatic EU region swap.
    const auto *eu868 = RegionPolicy::lookup(3);
    const auto *eu866 = RegionPolicy::lookup(29);
    const auto *euN868 = RegionPolicy::lookup(32);
    return eu868 && eu866 && euN868 &&
           appendProfilePresets(group, presetCapacity, eu868->profile) &&
           appendProfilePresets(group, presetCapacity, eu866->profile) &&
           appendProfilePresets(group, presetCapacity, euN868->profile);
  }
  return appendProfilePresets(group, presetCapacity, region.profile);
}

} // namespace detail

// Fill the caller's existing generated message in place. No allocation or
// scratch map is used. Capacity/invariant failure returns false and clears the
// output so a partially built catalogue cannot be emitted.
inline bool encode(meshtastic_LoRaRegionPresetMap &out) {
  out = {};
  const size_t groupCapacity = sizeof(out.groups) / sizeof(out.groups[0]);
  const size_t regionCapacity = sizeof(out.region_groups) / sizeof(out.region_groups[0]);
  const size_t presetCapacity = sizeof(out.groups[0].presets) / sizeof(out.groups[0].presets[0]);

  for (size_t i = 0; i < RegionPolicy::kRegionCount; ++i) {
    const RegionPolicy::Region *region = RegionPolicy::regionAt(i);
    if (!region || !region->profile || (region->profile->presetCount && !region->profile->presets))
      return detail::fail(out);
    if (region->code == 0) continue; // Stock default catalogue omits UNSET.
    if (out.region_groups_count >= regionCapacity) return detail::fail(out);

    int groupIndex = detail::findProfileGroup(out, *region);
    if (groupIndex < 0) {
      if (out.groups_count >= groupCapacity || out.groups_count > UINT8_MAX)
        return detail::fail(out);
      groupIndex = static_cast<int>(out.groups_count++);
      if (!detail::fillGroup(out.groups[groupIndex], *region, presetCapacity))
        return detail::fail(out);
      if (out.groups[groupIndex].presets_count == 0) return detail::fail(out);
    }

    auto &mapped = out.region_groups[out.region_groups_count++];
    mapped.region = static_cast<meshtastic_Config_LoRaConfig_RegionCode>(region->code);
    mapped.group_index = static_cast<uint8_t>(groupIndex);
  }

#if defined(USERPREFS_LORACONFIG_MODEM_PRESET)
  // Preserve stock's optional UNSET preset-intent entry for builds which
  // explicitly pin a modem preset at compile time. The default project does
  // not define this option.
  if (out.groups_count >= groupCapacity || out.region_groups_count >= regionCapacity)
    return detail::fail(out);
  const RegionPolicy::Region *unset = RegionPolicy::lookup(0);
  if (!unset || !unset->profile) return detail::fail(out);
  auto &group = out.groups[out.groups_count];
  group = {};
  group.presets_count = 1;
  group.presets[0] = static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(
      USERPREFS_LORACONFIG_MODEM_PRESET);
  group.default_preset = static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(
      USERPREFS_LORACONFIG_MODEM_PRESET);
  group.licensed_only = unset->profile->licensedOnly;
  auto &mapped = out.region_groups[out.region_groups_count++];
  mapped.region = static_cast<meshtastic_Config_LoRaConfig_RegionCode>(unset->code);
  mapped.group_index = static_cast<uint8_t>(out.groups_count++);
#endif

  return true;
}

} // namespace mt::radio::RegionCatalogue
