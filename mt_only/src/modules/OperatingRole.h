// SPDX-License-Identifier: GPL-3.0-only
// Pinned NodeDB::installRoleDefaults. Apply only when the committed role changes.
#pragma once
#include "TakRoleDefaults.h"
namespace mt::modules {
struct OperatingRole {
    static bool handles(uint32_t role){
        using namespace DeviceRolePolicy::Role;
        return role==TRACKER||role==SENSOR||role==TAK||role==CLIENT_HIDDEN||role==LOST_AND_FOUND;
    }
    static bool project(Config &core,ModuleSettings &modules,meshtastic_ModuleConfig *full=nullptr){
        using namespace DeviceRolePolicy::Role;
        const auto role=core.role();
        if(!handles(role))return false;
        if(role==CLIENT_HIDDEN){
            if(!full||full->which_payload_variant!=meshtastic_ModuleConfig_telemetry_tag)return false;
            constexpr uint32_t forever=INT32_MAX;
            auto position=core.gnssSettings();position.smart=false;position.broadcastSecs=forever;
            if(!core.setDeviceRole(role,DeviceRolePolicy::RebroadcastMode::LOCAL_ONLY)||
               !core.setNodeInfoSeconds(forever)||!core.setGnssSettings(position))return false;
            meshtastic_ModuleConfig legacy{};
            if(!modules.get(legacy,meshtastic_ModuleConfig_neighbor_info_tag))return false;
            legacy.payload_variant.neighbor_info.update_interval=forever;
            if(!modules.set(legacy)||!modules.get(legacy,meshtastic_ModuleConfig_telemetry_tag))return false;
            auto &all=full->payload_variant.telemetry;
            auto &t=legacy.payload_variant.telemetry;
            all.device_update_interval=t.device_update_interval=forever;
            all.environment_update_interval=t.environment_update_interval=forever;
            all.air_quality_interval=all.health_update_interval=forever;
            return modules.set(legacy)&&core.valid();
        }
        if(role==LOST_AND_FOUND){
            auto position=core.gnssSettings();position.smart=false;position.broadcastSecs=300;
            return core.setGnssSettings(position);
        }
        if(role==TAK){
            auto position=core.gnssSettings();position.smart=false;position.broadcastSecs=TakRoleDefaults::NodeInfoSeconds;
            position.positionFlags=TakRoleDefaults::PositionFlags;
            if(!core.setNodeInfoSeconds(TakRoleDefaults::NodeInfoSeconds)||!core.setGnssSettings(position))return false;
        }else core.setOwnerUnmessagable(true);
        meshtastic_ModuleConfig config{};
        if(!modules.get(config,meshtastic_ModuleConfig_telemetry_tag))return false;
        auto &t=config.payload_variant.telemetry;t.device_update_interval=role==TAK?TakRoleDefaults::TelemetrySeconds:3600;
        if(role==SENSOR){t.environment_measurement_enabled=true;t.environment_update_interval=300;}
        return modules.set(config)&&core.valid();
    }
};
}
