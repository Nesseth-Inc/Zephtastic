// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PositionQuery.h"
#include "meshtastic/telemetry.pb.h"
#include <float.h>
#include "Observed.h"
#include "PeerDeviceMetrics.h"
#include "HardwareSnapshot.h"
#include "stock/telemetry/ReportModules.h"
namespace mt {
// Remote Public DeviceMetrics are unauthenticated claims, never local sensor
// readings. Client cache values select the query but never enter its RF bytes.
class TelemetryQuery {
  static bool metricsValid(const meshtastic_DeviceMetrics &m) {
    // Ordered bounds reject NaN and both infinities without requiring the full
    // C++ math library, which is absent in Zephyr's minimal C++ configuration.
    return (!m.has_voltage||(m.voltage>=0&&m.voltage<=FLT_MAX))&&
      (!m.has_channel_utilization||(m.channel_utilization>=0&&m.channel_utilization<=100))&&
      (!m.has_air_util_tx||(m.air_util_tx>=0&&m.air_util_tx<=100));
  }
  static bool decode(const uint8_t *payload,size_t size,meshtastic_Telemetry &t) {
    if(!payload||!size||size>233)return false;
    auto in=pb_istream_from_buffer(payload,size);
    if(!pb_decode(&in,meshtastic_Telemetry_fields,&t)||in.bytes_left||
      t.which_variant!=meshtastic_Telemetry_device_metrics_tag||!metricsValid(t.variant.device_metrics))return false;
    uint8_t canonical[233]{};auto out=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&out,meshtastic_Telemetry_fields,&t)&&out.bytes_written==size&&!memcmp(canonical,payload,size);
  }
public:
  // Decode only the already-admitted response's canonical nonempty snapshot.
  // The receive caller still owns peer/request/duplicate admission.
  static bool cachedMetrics(const uint8_t *payload,size_t size,PeerDeviceMetrics &out){
    meshtastic_Telemetry t{};
    return decode(payload,size,t)&&out.assign(t.variant.device_metrics);
  }
  // Shared uptime/airtime encoder: uptime_seconds plus optional fractional
  // air_util_tx percent (float wire type). airUtilTx is omitted when
  // unavailable, nonfinite, or out of range; uptime semantics are unchanged.
  // Airtime is estimated TX airtime, not a calibrated measurement.
  static bool uptimeResponse(uint32_t uptime,uint32_t request,const zt::Observed<float> &airUtilTx,const zt::Observed<float> &channelUtilization,uint8_t *data,size_t capacity,size_t &size,const HardwareSnapshot &hardware={},uint32_t utc=0) {
    size=0;
    if(!request)return false;
    meshtastic_Telemetry report{};
    stock::telemetry::deviceReport(report,utc,uptime,airUtilTx,channelUtilization,hardware);
    return stock::telemetry::encodeReport(report,request,data,capacity,size);
  }
  static bool uptimeResponse(uint32_t uptime,uint32_t request,const zt::Observed<float> &airUtilTx,uint8_t *data,size_t capacity,size_t &size) {
    return uptimeResponse(uptime,request,airUtilTx,{},data,capacity,size);
  }
  static bool uptimeResponse(uint32_t uptime,uint32_t request,uint8_t *data,size_t capacity,size_t &size) {
    return uptimeResponse(uptime,request,{},data,capacity,size);
  }
  static bool request(const uint8_t *payload,size_t size) {
    // Stock Python's cold cache emits no selector at all. A warm cache emits
    // its local DeviceMetrics. Both become the same empty remote selector.
    if(!size)return true;
    meshtastic_Telemetry t{};return decode(payload,size,t)&&!t.time;
  }
  static bool responseShape(const bench::RouteMetadata &m) {
    return m.portnum==meshtastic_PortNum_TELEMETRY_APP&&m.request_id&&!m.wantResponse&&
      !m.reply_id&&!m.emoji&&!m.signatureLength&&m.hasBitfield&&!(m.bitfield&~1u);
  }
  static bool response(const bench::RouteMetadata &m,uint8_t *data,size_t capacity,size_t &size) {
    size=0;if(!responseShape(m))return false;
    meshtastic_Telemetry t{};if(!decode(m.payload,m.payloadLength,t))return false;
    const auto &v=t.variant.device_metrics;
    if(!v.has_battery_level&&!v.has_voltage&&!v.has_channel_utilization&&!v.has_air_util_tx&&!v.has_uptime_seconds)return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.request_id=m.request_id;
    d.has_bitfield=true;d.bitfield=m.bitfield;d.payload.size=m.payloadLength;memcpy(d.payload.bytes,m.payload,m.payloadLength);
    auto out=pb_ostream_from_buffer(data,capacity);
    if(!pb_encode(&out,meshtastic_Data_fields,&d))return false;
    size=out.bytes_written;return true;
  }
};
}
