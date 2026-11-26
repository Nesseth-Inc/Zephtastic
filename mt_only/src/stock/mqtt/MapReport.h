// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, MQTT::perhapsReportToMap in src/mqtt/MQTT.cpp.
#pragma once
#include <meshtastic/mqtt.pb.h>
#include <stdint.h>
#include <string.h>

namespace mt::stock::MQTT {
inline uint32_t mapPrecision(uint32_t precision) {
    return precision < 12 || precision > 15 ? 14 : precision;
}

inline int32_t mapCoordinate(int32_t coordinate, uint32_t precision) {
    const uint32_t masked = uint32_t(coordinate) & (UINT32_MAX << (32 - precision));
    return static_cast<int32_t>(masked + (uint32_t(1) << (31 - precision)));
}

// ZT supplies validated metadata and a bounded publish sink after checking
// location consent, primary event-channel policy, transport and cadence.
template<class Source>
void fillMapReport(meshtastic_MapReport &mapReport, const Source &source, uint32_t configuredPrecision) {
    const auto map_position_precision = mapPrecision(configuredPrecision);
    // The donor's short name and version are fixed arrays. The adapter accepts
    // C strings, so bound both by the destination and leave a terminator.
    strncpy(mapReport.long_name, source.longName, sizeof(mapReport.long_name) - 1);
    strncpy(mapReport.short_name, source.shortName, sizeof(mapReport.short_name) - 1);
    strncpy(mapReport.firmware_version, source.firmwareVersion, sizeof(mapReport.firmware_version) - 1);
    mapReport.role = static_cast<meshtastic_Config_DeviceConfig_Role>(source.role);
    mapReport.hw_model = static_cast<meshtastic_HardwareModel>(source.hwModel);
    mapReport.region = static_cast<meshtastic_Config_LoRaConfig_RegionCode>(source.region);
    mapReport.modem_preset = static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(source.modemPreset);
    mapReport.has_default_channel = source.hasDefaultChannel;
    mapReport.has_opted_report_location = true;
    mapReport.latitude_i = mapCoordinate(source.latitudeI, map_position_precision);
    mapReport.longitude_i = mapCoordinate(source.longitudeI, map_position_precision);
    mapReport.altitude = source.altitude;
    mapReport.position_precision = map_position_precision;
    mapReport.num_online_local_nodes = source.numOnlineLocalNodes;
}
}
