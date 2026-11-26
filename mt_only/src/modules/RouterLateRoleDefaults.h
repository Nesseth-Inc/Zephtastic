// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Config.h"
#include "../ModuleSettings.h"
#include "../DeviceRolePolicy.h"

namespace mt::modules {
// Projection of the pinned stock ROUTER_LATE role-transition defaults.
// It does not write storage and leaves radio, position, and channel settings
// untouched; the owner coordinates core/module persistence.
struct RouterLateRoleDefaults {
  enum class Result : uint8_t { Applied, SameRole, NotRouterLateTransition, InvalidInput, UnsupportedSettings };
  struct Projection { Config core{}; ModuleSettings modules{}; };
  static constexpr uint32_t RoleRouterLate=11u;
  static constexpr uint32_t TelemetrySeconds=86400u;

  static Result project(uint32_t oldRole,uint32_t newRole,const Config &currentCore,
                        const ModuleSettings &currentModules,Projection &out,
                        uint32_t requestedRebroadcast=UINT32_MAX) {
    if(!currentCore.valid()||!currentModules.valid()||oldRole!=currentCore.role())
      return Result::InvalidInput;
    if(oldRole==newRole) { out={currentCore,currentModules}; return Result::SameRole; }
    if(newRole!=RoleRouterLate||!DeviceRolePolicy::supportedRole(newRole))
      return Result::NotRouterLateTransition;

    Config core=currentCore;
    const uint32_t rebroadcast=requestedRebroadcast==UINT32_MAX?
      core.rebroadcastMode():requestedRebroadcast;
    if(!core.setDeviceRole(newRole,rebroadcast))return Result::UnsupportedSettings;
    return applyDefaults(core,currentModules,out);
  }

  // Admin first installs the complete accepted DeviceConfig, then installs
  // defaults for the new role. Other fields in that same request survive.
  static Result projectRequest(const Config &requested,const ModuleSettings &currentModules,Projection &out) {
    if(!requested.valid()||!currentModules.valid())return Result::InvalidInput;
    if(requested.role()!=RoleRouterLate)return Result::NotRouterLateTransition;
    Config core=requested;
    return applyDefaults(core,currentModules,out);
  }

private:
  static Result applyDefaults(Config &core,const ModuleSettings &currentModules,Projection &out) {
    ModuleSettings modules=currentModules;
    core.setOwnerUnmessagable(true);
    meshtastic_ModuleConfig telemetry{};
    if(!modules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag))
      return Result::UnsupportedSettings;
    telemetry.payload_variant.telemetry.device_update_interval=TelemetrySeconds;
    if(!modules.set(telemetry)||!core.valid()||!modules.valid())
      return Result::UnsupportedSettings;
    out={core,modules};
    return Result::Applied;
  }
};
} // namespace mt::modules
