// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TelemetryQuery.h"
#include "NodeCache.h"

namespace mt {
// A remote statistics snapshot is a peer claim, never a local reading
// or a DeviceMetrics cache entry. Local production uses only explicit readings.
struct LocalStatsObservation {
  uint32_t packetsTx=0;
  uint16_t totalNodes=0,onlineNodes=0;
  uint32_t packetsRx=0,badRx=0,duplicates=0,relayed=0,canceled=0;
  uint32_t heapTotal=0,heapFree=0;
  int8_t noiseFloor=0;
};
class LocalStatsQuery {
  // LocalStats uses proto3 scalars: positive zero and unavailable readings
  // share the omitted default; nanopb preserves a measured negative-zero sign.
  // Each field comes from its owner. Unknown measurements retain the wire default.
  static bool encode(uint32_t uptime,uint32_t request,
    const zt::Observed<float> &airUtilTx,const zt::Observed<float> &channelUtilization,
    uint8_t *data,size_t capacity,size_t &size,const LocalStatsObservation *observed=nullptr,uint32_t utc=0) {
    size=0;if(!data)return false;
    meshtastic_Telemetry t{};t.time=utc;t.which_variant=meshtastic_Telemetry_local_stats_tag;
    t.variant.local_stats.uptime_seconds=uptime;
    if(observed) {
      t.variant.local_stats.num_packets_tx=observed->packetsTx;
      auto &stats=t.variant.local_stats;
      stats.num_total_nodes=observed->totalNodes;stats.num_online_nodes=observed->onlineNodes;
      stats.num_packets_rx=observed->packetsRx;stats.num_packets_rx_bad=observed->badRx;
      stats.num_rx_dupe=observed->duplicates;stats.num_tx_relay=observed->relayed;
      stats.num_tx_relay_canceled=observed->canceled;
      stats.noise_floor=observed->noiseFloor;
      stats.heap_total_bytes=observed->heapTotal;stats.heap_free_bytes=observed->heapFree;
    }
    if(airUtilTx.has_value()&&*airUtilTx>=0.0f&&*airUtilTx<=100.0f)
      t.variant.local_stats.air_util_tx=*airUtilTx;
    if(channelUtilization.has_value()&&*channelUtilization>=0.0f&&*channelUtilization<=100.0f)
      t.variant.local_stats.channel_utilization=*channelUtilization;
    return stock::telemetry::encodeReport(t,request,data,capacity,size);
  }
public:
  static bool uptimeResponse(uint32_t uptime,uint32_t request,
    const zt::Observed<float> &air,const zt::Observed<float> &channel,
    uint8_t *data,size_t capacity,size_t &size,const LocalStatsObservation *observed=nullptr,uint32_t utc=0){
    size=0;return request&&encode(uptime,request,air,channel,data,capacity,size,observed,utc);
  }
  static bool announcement(uint32_t uptime,const zt::Observed<float>&air,const zt::Observed<float>&channel,
    uint8_t*data,size_t capacity,size_t&size,const LocalStatsObservation&observed,uint32_t utc=0){
    return encode(uptime,0,air,channel,data,capacity,size,&observed,utc);
  }
  // Only the empty LocalStats selector is accepted; caller counters and time
  // are never copied into an outgoing request.
  static bool request(const uint8_t *payload,size_t size) {
    return payload&&size==2&&payload[0]==0x32&&payload[1]==0;
  }
  static bool responsePayload(const uint8_t *payload,size_t size) {
    if(!payload||!size||size>233)return false;
    meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(payload,size);
    if(!pb_decode(&in,meshtastic_Telemetry_fields,&t)||in.bytes_left||
       t.which_variant!=meshtastic_Telemetry_local_stats_tag)return false;
    const auto &stats=t.variant.local_stats;
    if(!(stats.channel_utilization>=0&&stats.channel_utilization<=100)||
       !(stats.air_util_tx>=0&&stats.air_util_tx<=100))return false;
    uint8_t canonical[233]{};auto out=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&out,meshtastic_Telemetry_fields,&t)&&out.bytes_written==size&&
      !memcmp(canonical,payload,size);
  }
  static bool response(const bench::RouteMetadata &m,uint8_t *data,size_t capacity,size_t &size) {
    size=0;
    if(!TelemetryQuery::responseShape(m)||!responsePayload(m.payload,m.payloadLength))return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.request_id=m.request_id;
    d.has_bitfield=true;d.bitfield=m.bitfield;d.payload.size=m.payloadLength;
    memcpy(d.payload.bytes,m.payload,m.payloadLength);
    auto out=pb_ostream_from_buffer(data,capacity);
    if(!pb_encode(&out,meshtastic_Data_fields,&d))return false;
    size=out.bytes_written;return true;
  }
};
}
