// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Config.h"
#include "../DeviceRolePolicy.h"
#include "../ModuleSettings.h"

// Keep the exact Default.h expressions private to this translation unit. The
// three included .inc files are the exact pinned NodeDB.cpp statement regions;
// only their surrounding typed facade is ZephTastic-specific.
#pragma push_macro("ONE_DAY")
#pragma push_macro("MAX_INTERVAL")
#pragma push_macro("IF_ROUTER")
#pragma push_macro("default_gps_update_interval")
#pragma push_macro("default_telemetry_broadcast_interval_secs")
#pragma push_macro("default_broadcast_interval_secs")
#pragma push_macro("default_wait_bluetooth_secs")
#pragma push_macro("default_sds_secs")
#pragma push_macro("default_ls_secs")
#pragma push_macro("default_min_wake_secs")
#pragma push_macro("default_screen_on_secs")
#undef ONE_DAY
#undef MAX_INTERVAL
#undef IF_ROUTER
#undef default_gps_update_interval
#undef default_telemetry_broadcast_interval_secs
#undef default_broadcast_interval_secs
#undef default_wait_bluetooth_secs
#undef default_sds_secs
#undef default_ls_secs
#undef default_min_wake_secs
#undef default_screen_on_secs
#include "RouterRoleDefaults.stockDefaults.inc"

namespace mt::modules {
struct RouterRoleDefaults {
  enum class Result : uint8_t { Applied, SameRole, NotRouterTransition, InvalidInput, UnsupportedSettings };
  enum Unsupported : uint8_t { NoUnsupportedFields=0, PowerConfigFields=1, ModuleTelemetryFields=2 };
  struct Projection {
    Config core{};
    ModuleSettings modules{};
    uint8_t unsupported=NoUnsupportedFields;
    meshtastic_Config_PowerConfig power{};
    meshtastic_ModuleConfig_TelemetryConfig telemetry{};
    uint32_t paxcounterInterval=0;
  };
  static constexpr uint32_t RoleRouter=DeviceRolePolicy::Role::ROUTER;

  // Minimal protobuf-shaped objects let the pinned stock statements execute
  // unchanged. They are temporary values only; mapping below writes only
  // currently represented Config/ModuleSettings fields.
  struct StockConfig {
    struct Device { uint32_t role=0,rebroadcast_mode=0; } device;
    struct Position { uint32_t gps_update_interval=0,position_broadcast_secs=0; } position;
    struct Power {
      uint32_t ls_secs=0,min_wake_secs=0,sds_secs=0,wait_bluetooth_secs=0;
      bool is_power_saving=false;
    } power;
    struct Display { uint32_t screen_on_secs=0; } display;
  };
  struct StockModules {
    struct Telemetry {
      uint32_t device_update_interval=0,environment_update_interval=0;
      uint32_t air_quality_interval=0,power_update_interval=0,health_update_interval=0;
      bool environment_measurement_enabled=false,environment_screen_enabled=false;
      bool air_quality_enabled=false,air_quality_screen_enabled=false;
    } telemetry;
    struct NeighborInfo { uint32_t update_interval=0; } neighbor_info;
    struct Paxcounter { uint32_t paxcounter_update_interval=0; } paxcounter;
  };
  struct StockOwner { bool has_is_unmessagable=false,is_unmessagable=false; };

  static Result project(uint32_t oldRole,const Config &candidateCore,
                        const ModuleSettings &currentModules,Projection &out,
                        const meshtastic_ModuleConfig_TelemetryConfig *fullTelemetry=nullptr,
                        const meshtastic_Config_PowerConfig *fullPower=nullptr) {
    if(!candidateCore.valid()||!currentModules.valid())return Result::InvalidInput;
    if(candidateCore.role()==oldRole) {
      Projection same{candidateCore,currentModules,NoUnsupportedFields};out=same;return Result::SameRole;
    }
    if(candidateCore.role()!=RoleRouter||!DeviceRolePolicy::supportedRole(RoleRouter))
      return Result::NotRouterTransition;

    auto core=candidateCore;
    auto gnss=core.gnssSettings();
    meshtastic_ModuleConfig telemetry{},neighbor{};
    if(!currentModules.get(telemetry,meshtastic_ModuleConfig_telemetry_tag)||
       !currentModules.get(neighbor,meshtastic_ModuleConfig_neighbor_info_tag))return Result::UnsupportedSettings;

    StockConfig config{};
    StockModules moduleConfig{};
    StockOwner owner{true,core.ownerUnmessagable()};
    config.device.role=core.role();
    config.device.rebroadcast_mode=core.rebroadcastMode();
    config.position.gps_update_interval=gnss.updateSecs;
    config.position.position_broadcast_secs=gnss.broadcastSecs;
    config.display.screen_on_secs=core.displaySeconds();
    const auto role=config.device.role;
    auto &currentTelemetry=telemetry.payload_variant.telemetry;
    moduleConfig.telemetry.device_update_interval=currentTelemetry.device_update_interval;
    moduleConfig.telemetry.environment_update_interval=currentTelemetry.environment_update_interval;
    moduleConfig.telemetry.power_update_interval=currentTelemetry.power_update_interval;
    moduleConfig.telemetry.environment_measurement_enabled=currentTelemetry.environment_measurement_enabled;
    if(fullTelemetry){
      moduleConfig.telemetry.environment_screen_enabled=fullTelemetry->environment_screen_enabled;
      moduleConfig.telemetry.air_quality_interval=fullTelemetry->air_quality_interval;
      moduleConfig.telemetry.air_quality_enabled=fullTelemetry->air_quality_enabled;
      moduleConfig.telemetry.air_quality_screen_enabled=fullTelemetry->air_quality_screen_enabled;
      moduleConfig.telemetry.health_update_interval=fullTelemetry->health_update_interval;
    }

    moduleConfig.neighbor_info.update_interval=neighbor.payload_variant.neighbor_info.update_interval;

    auto initConfigIntervals=[&]() {
#include "RouterRoleDefaults.initConfigIntervals.inc"
    };
    auto initModuleConfigIntervals=[&]() {
#include "RouterRoleDefaults.initModuleConfigIntervals.inc"
    };
    auto installRoleDefaults=[&]() {
#include "RouterRoleDefaults.installRouterRoleDefaults.inc"
    };
    installRoleDefaults();

    gnss.updateSecs=config.position.gps_update_interval;
    gnss.broadcastSecs=config.position.position_broadcast_secs;
    if(!core.setGnssSettings(gnss)||
       !core.setDisplay(config.display.screen_on_secs,core.displayFlip(),core.displayCarouselSeconds())||
       !core.setDeviceRole(RoleRouter,config.device.rebroadcast_mode))return Result::UnsupportedSettings;
    core.setOwnerUnmessagable(owner.is_unmessagable);

    currentTelemetry.device_update_interval=moduleConfig.telemetry.device_update_interval;
    currentTelemetry.environment_measurement_enabled=moduleConfig.telemetry.environment_measurement_enabled;
    currentTelemetry.environment_update_interval=moduleConfig.telemetry.environment_update_interval;
    currentTelemetry.power_update_interval=moduleConfig.telemetry.power_update_interval;
    neighbor.payload_variant.neighbor_info.update_interval=moduleConfig.neighbor_info.update_interval;
    auto projectedModules=currentModules;
    if(!projectedModules.set(telemetry)||!projectedModules.set(neighbor)||
       !core.valid()||!projectedModules.valid())return Result::UnsupportedSettings;

    Projection candidate{core,projectedModules,uint8_t(PowerConfigFields|ModuleTelemetryFields)};
    if(fullPower){
      candidate.power=*fullPower;candidate.power.ls_secs=config.power.ls_secs;
      candidate.power.min_wake_secs=config.power.min_wake_secs;candidate.power.sds_secs=config.power.sds_secs;
      candidate.power.wait_bluetooth_secs=config.power.wait_bluetooth_secs;
#if defined(USE_POWERSAVE)
      candidate.power.is_power_saving=config.power.is_power_saving;
#endif
      candidate.unsupported&=uint8_t(~PowerConfigFields);
    }
    if(fullTelemetry){
      candidate.telemetry=*fullTelemetry;
      candidate.telemetry.device_update_interval=moduleConfig.telemetry.device_update_interval;
      candidate.telemetry.environment_measurement_enabled=moduleConfig.telemetry.environment_measurement_enabled;
      candidate.telemetry.environment_update_interval=moduleConfig.telemetry.environment_update_interval;
      candidate.telemetry.environment_screen_enabled=moduleConfig.telemetry.environment_screen_enabled;
      candidate.telemetry.air_quality_enabled=moduleConfig.telemetry.air_quality_enabled;
      candidate.telemetry.air_quality_interval=moduleConfig.telemetry.air_quality_interval;
      candidate.telemetry.air_quality_screen_enabled=moduleConfig.telemetry.air_quality_screen_enabled;
      candidate.telemetry.power_update_interval=moduleConfig.telemetry.power_update_interval;
      candidate.telemetry.health_update_interval=moduleConfig.telemetry.health_update_interval;
      candidate.paxcounterInterval=moduleConfig.paxcounter.paxcounter_update_interval;
      candidate.unsupported&=uint8_t(~ModuleTelemetryFields);
    }

    out=candidate;
    return Result::Applied;
  }
};
} // namespace mt::modules

#pragma pop_macro("default_screen_on_secs")
#pragma pop_macro("default_min_wake_secs")
#pragma pop_macro("default_ls_secs")
#pragma pop_macro("default_sds_secs")
#pragma pop_macro("default_wait_bluetooth_secs")
#pragma pop_macro("default_broadcast_interval_secs")
#pragma pop_macro("default_telemetry_broadcast_interval_secs")
#pragma pop_macro("default_gps_update_interval")
#pragma pop_macro("IF_ROUTER")
#pragma pop_macro("MAX_INTERVAL")
#pragma pop_macro("ONE_DAY")
