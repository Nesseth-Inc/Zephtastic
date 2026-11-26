// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d: DeviceTelemetry::getDeviceTelemetry,
// EnvironmentTelemetry::getEnvironmentTelemetry and PowerTelemetry::getPowerTelemetry.
#pragma once
#include "../../HardwareSnapshot.h"
#include "../../TxAdmission.h"
#include "meshtastic/config.pb.h"
#include "../../environment/EnvironmentMetricsAdapter.h"
#include "../../power/PowerMetricsAdapter.h"
#include "../../power/PowerReadings.h"
#include "meshtastic/mesh.pb.h"
#include <pb_encode.h>

namespace mt::stock::telemetry {
// Source runOnce admission. PowerTelemetry gates only on transmit airtime.
inline bool deviceMeshAllowed(unsigned role,float channel,bool airtimeAllowed) {
    const bool impolite=role==meshtastic_Config_DeviceConfig_Role_SENSOR ||
                        role==meshtastic_Config_DeviceConfig_Role_ROUTER ||
                        role==meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    return role!=meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN && airtimeAllowed &&
           TxAdmission::channel(true,channel,!impolite)==TxAdmission::Decision::Allow;
}
inline bool environmentMeshAllowed(unsigned role,float channel,bool airtimeAllowed) {
    return airtimeAllowed && TxAdmission::channel(true,channel,role!=meshtastic_Config_DeviceConfig_Role_SENSOR)==TxAdmission::Decision::Allow;
}

inline void deviceReport(meshtastic_Telemetry &out,uint32_t utc,uint32_t uptime,
                         const zt::Observed<float> &air,const zt::Observed<float> &channel,
                         const HardwareSnapshot &hardware) {
    out.time=utc;
    out.which_variant=meshtastic_Telemetry_device_metrics_tag;
    auto &m=out.variant.device_metrics;
    m={};
    hardware.apply(m);
    m.has_uptime_seconds=true;
    m.uptime_seconds=uptime;
    if(air && *air>=0 && *air<=100){m.has_air_util_tx=true;m.air_util_tx=*air;}
    if(channel && *channel>=0 && *channel<=100){m.has_channel_utilization=true;m.channel_utilization=*channel;}
}

inline bool environmentReport(meshtastic_Telemetry &out,uint32_t utc,const zt::telemetry::EnvironmentReadings &sample) {
    out.time=utc;
    out.which_variant=meshtastic_Telemetry_environment_metrics_tag;
    auto &m=out.variant.environment_metrics;
    m=zt::telemetry::toMeshtasticEnvironmentMetrics(sample);
    return zt::telemetry::hasEnvironmentMetrics(m);
}

inline bool powerReport(meshtastic_Telemetry &out,uint32_t utc,const zt::telemetry::PowerReadings &sample) {
    out.time=utc;
    out.which_variant=meshtastic_Telemetry_power_metrics_tag;
    out.variant.power_metrics=zt::telemetry::buildPowerMetrics(sample.voltage,sample.current);
    return zt::telemetry::hasPowerMetrics(out.variant.power_metrics);
}

inline bool encodeReport(const meshtastic_Telemetry &telemetry,uint32_t request,uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!out || !capacity)return false;
    meshtastic_Data data{};
    data.portnum=meshtastic_PortNum_TELEMETRY_APP;
    data.request_id=request;
    data.has_bitfield=true;
    auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(!pb_encode(&payload,meshtastic_Telemetry_fields,&telemetry))return false;
    data.payload.size=payload.bytes_written;
    auto stream=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&stream,meshtastic_Data_fields,&data))return false;
    size=stream.bytes_written;
    return true;
}
}
