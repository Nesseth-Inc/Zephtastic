// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Config.h"
#include "../ModuleSettings.h"
#include "../DeviceRolePolicy.h"
#include "../gnss/GnssSettings.h"

namespace mt::modules {
// Pure projection of the pinned stock AdminModule role-transition defaults.
// It never persists settings or changes owner identity. The projected core
// includes the Owner metadata flag; the caller coordinates core/module saves.
struct TakRoleDefaults {
  enum class Result : uint8_t { Applied, SameRole, NotTakTrackerTransition, InvalidInput, UnsupportedSettings };
  struct Projection {
    Config core{};
    ModuleSettings modules{};
    bool ownerUnmessagableRequired = false;
  };

  static constexpr uint32_t RoleTakTracker = 10;
  static constexpr uint32_t NodeInfoSeconds = 86400;
  static constexpr uint32_t TelemetrySeconds = 86400;
  static constexpr uint32_t PositionSeconds = 180;
  static constexpr uint32_t SmartDistanceMeters = 20;
  static constexpr uint32_t SmartMinimumIntervalSeconds = 15;
  static constexpr uint32_t PositionFlags =
      uint32_t(meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE) |
      uint32_t(meshtastic_Config_PositionConfig_PositionFlags_SPEED) |
      uint32_t(meshtastic_Config_PositionConfig_PositionFlags_HEADING) |
      uint32_t(meshtastic_Config_PositionConfig_PositionFlags_DOP);

  // On failure `out` is untouched. Same-role sets deliberately do not replay
  // role defaults, matching stock AdminModule::handleSetConfig().
  static Result project(uint32_t oldRole, uint32_t newRole, const Config &currentCore,
                        const ModuleSettings &currentModules, Projection &out,
                        uint32_t requestedRebroadcast = UINT32_MAX) {
#ifdef CONFIG_MT_TAK_TRACKER
    if (!currentCore.valid() || !currentModules.valid() || oldRole != currentCore.role())
      return Result::InvalidInput;
    if (oldRole == newRole) {
      Projection candidate{currentCore, currentModules, false};
      out = candidate;
      return Result::SameRole;
    }
    if (newRole != RoleTakTracker || !DeviceRolePolicy::supportedRole(newRole))
      return Result::NotTakTrackerTransition;

    Config core = currentCore;
    // Stock NodeDB role defaults do not assign rebroadcast mode.
    const uint32_t rebroadcast=requestedRebroadcast==UINT32_MAX?
      core.rebroadcastMode():requestedRebroadcast;
    if (!core.setDeviceRole(newRole, rebroadcast))
      return Result::UnsupportedSettings;
    return applyDefaults(core, currentModules, out);
#else
    (void)oldRole; (void)newRole; (void)currentCore; (void)currentModules; (void)out;
    (void)requestedRebroadcast;
    return Result::NotTakTrackerTransition;
#endif
  }
#ifdef CONFIG_MT_TAK_TRACKER
  static Result projectRequest(const Config &requested, const ModuleSettings &currentModules, Projection &out) {
    if (!requested.valid() || !currentModules.valid())
      return Result::InvalidInput;
    if (requested.role() != RoleTakTracker)
      return Result::NotTakTrackerTransition;
    Config core = requested;
    return applyDefaults(core, currentModules, out);
  }

private:
  static Result applyDefaults(Config &core, const ModuleSettings &currentModules, Projection &out) {
    ModuleSettings modules = currentModules;
    core.setOwnerUnmessagable(true);
    if (!core.setNodeInfoSeconds(NodeInfoSeconds))
      return Result::UnsupportedSettings;

    auto position = core.gnssSettings();
    position.smart = true;
    position.positionFlags = PositionFlags;
    position.broadcastSecs = PositionSeconds;
    position.smartMinDistance = SmartDistanceMeters;
    position.smartMinInterval = SmartMinimumIntervalSeconds;
    if (!core.setGnssSettings(position))
      return Result::UnsupportedSettings;

    meshtastic_ModuleConfig telemetry{};
    if (!modules.get(telemetry, meshtastic_ModuleConfig_telemetry_tag))
      return Result::UnsupportedSettings;
    telemetry.payload_variant.telemetry.device_update_interval = TelemetrySeconds;
    if (!modules.set(telemetry) || !core.valid() || !modules.valid())
      return Result::UnsupportedSettings;

    Projection candidate{core, modules, true};
    out = candidate;
    return Result::Applied;
  }
#endif
};
} // namespace mt::modules
