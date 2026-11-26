// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtCodec.h"
#include "stock/modules/TraceRouteModule.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

namespace mt {
// Bounded RouteDiscovery endpoint and Public-unicast transit policy. Payloads
// never create trust or next-hop state; Service owns the only transit entry.
class TraceRoute {
public:
  using Discovery=meshtastic_RouteDiscovery;
  enum class TransitResult:uint8_t {Invalid,Mutated,AlreadyVisited};
  static constexpr uint32_t UnknownNode=0xffffffffu;
  static constexpr int8_t UnknownSnr=-128;
  static constexpr size_t MaxPayload=233;
private:
  static bool decode(const bench::RouteMetadata &m,Discovery &out) {
    out=meshtastic_RouteDiscovery_init_zero;
    if(!m.traceRouteValid||m.payloadLength>MaxPayload)return false;
    auto stream=pb_istream_from_buffer(m.payload,m.payloadLength);
    return pb_decode(&stream,meshtastic_RouteDiscovery_fields,&out)&&stream.bytes_left==0;
  }
  using Stock=stock::TraceRouteModule;
  static bool fill(Stock::Path path,unsigned hops,int8_t snr) {
    if(path.count>7||path.signalCount>path.count||hops>7)return false;
    Stock::insertUnknownHops(path,hops);
    Stock::appendMyIDandSNR(path,snr,0,true);
    return true;
  }
  static bool encodePayload(const Discovery &route,uint8_t *out,size_t capacity,size_t &size) {
    size=0;if(!out||!capacity)return false;
    auto stream=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&stream,meshtastic_RouteDiscovery_fields,&route))return false;
    size=stream.bytes_written;return size<=MaxPayload;
  }
  static bool contains(const uint32_t *route,pb_size_t count,uint32_t node) {
    for(pb_size_t i=0;i<count;++i)if(route[i]==node)return true;
    return false;
  }
  static TransitResult transitDirection(Stock::Path path,unsigned hops,int8_t snr,uint32_t ours) {
    if(!ours||ours==UnknownNode||path.count>6||path.signalCount>path.count||hops>7)return TransitResult::Invalid;
    if(contains(path.nodes,path.count,ours))return TransitResult::AlreadyVisited;
    Stock::insertUnknownHops(path,hops);
    Stock::appendMyIDandSNR(path,snr,ours,false);
    return TransitResult::Mutated;
  }
public:
  static bool requestShape(const bench::RouteMetadata &m) {
    return m.portnum==meshtastic_PortNum_TRACEROUTE_APP&&m.traceRouteValid&&m.wantResponse&&
      !m.request_id&&!m.reply_id&&!m.emoji&&!m.signatureLength&&m.hasBitfield&&(m.bitfield==0||m.bitfield==2);
  }
  static bool responseShape(const bench::RouteMetadata &m) {
    return m.portnum==meshtastic_PortNum_TRACEROUTE_APP&&m.traceRouteValid&&!m.wantResponse&&
      m.request_id&&!m.reply_id&&!m.emoji&&!m.signatureLength&&m.hasBitfield&&!m.bitfield;
  }
  // Decode the exact supported endpoint/transit semantic shape without
  // changing it. Wire canonicality remains MtCodec's responsibility.
  static bool semantic(const bench::RouteMetadata &m,Discovery &route) {
    if(!decode(m,route))return false;
    if(requestShape(m))
      return route.route_count<=7&&route.snr_towards_count<=route.route_count&&
        !route.route_back_count&&!route.snr_back_count;
    if(responseShape(m))
      return route.route_count<=7&&route.snr_towards_count==route.route_count+1&&
        route.route_back_count<=7&&route.snr_back_count<=route.route_back_count;
    return false;
  }
  static bool sameSemantic(const Discovery &a,const Discovery &b) {
    if(a.route_count!=b.route_count||a.snr_towards_count!=b.snr_towards_count||
       a.route_back_count!=b.route_back_count||a.snr_back_count!=b.snr_back_count)return false;
    return !memcmp(a.route,b.route,a.route_count*sizeof(a.route[0]))&&
      !memcmp(a.snr_towards,b.snr_towards,a.snr_towards_count*sizeof(a.snr_towards[0]))&&
      !memcmp(a.route_back,b.route_back,a.route_back_count*sizeof(a.route_back[0]))&&
      !memcmp(a.snr_back,b.snr_back,a.snr_back_count*sizeof(a.snr_back[0]));
  }
  // Relay-side pinned behavior. Requests grow the forward arrays; responses
  // grow only the reverse arrays. An already-present local ID is observation
  // evidence for RoutingCore only and must never be appended a second time.
  static TransitResult transit(const bench::RouteMetadata &m,uint32_t ours,unsigned hop,unsigned start,int8_t snr,
                               uint8_t *out,size_t capacity,size_t &size) {
    size=0;if(hop==0||hop>start||start>7)return TransitResult::Invalid;
    Discovery route;if(!semantic(m,route))return TransitResult::Invalid;
    const auto result=transitDirection(Stock::path(route,requestShape(m)),start-hop,snr,ours);
    if(result!=TransitResult::Mutated)return result;
    return encodePayload(route,out,capacity,size)?TransitResult::Mutated:TransitResult::Invalid;
  }
  static bool data(const bench::RouteMetadata &shape,const uint8_t *payload,size_t payloadSize,
                   uint8_t *out,size_t capacity,size_t &size) {
    if(requestShape(shape))return requestData(payload,payloadSize,out,capacity,size);
    if(responseShape(shape))return responseData(payload,payloadSize,shape.request_id,out,capacity,size);
    size=0;return false;
  }
  // Destination-side pinned behavior: preserve a bounded forward route, add
  // unknown transit positions implied by the RF header, then append our SNR.
  static bool destination(const bench::RouteMetadata &m,unsigned hop,unsigned start,int8_t snr,
                          uint8_t *out,size_t capacity,size_t &size) {
    size=0;Discovery route;
    if(!requestShape(m)||hop>start||!decode(m,route)||route.route_back_count||route.snr_back_count||
       !fill(Stock::path(route,true),start-hop,snr))return false;
    return encodePayload(route,out,capacity,size);
  }
  // Origin-side pinned behavior: require a complete forward result, preserve a
  // bounded asymmetric reverse route, fill unknown transit positions, append
  // our SNR, and emit canonical nanopb bytes for the client callback.
  static bool origin(const bench::RouteMetadata &m,unsigned hop,unsigned start,int8_t snr,
                     uint8_t *out,size_t capacity,size_t &size) {
    size=0;Discovery route;
    if(!responseShape(m)||hop>start||!decode(m,route)||route.route_count>7||
       route.snr_towards_count!=route.route_count+1||
       !fill(Stock::path(route,false),start-hop,snr))return false;
    return encodePayload(route,out,capacity,size);
  }
  static bool requestData(const uint8_t *payload,size_t payloadSize,uint8_t *out,size_t capacity,size_t &size) {
    size=0;if(!out||payloadSize>MaxPayload||(payloadSize&&!payload))return false;
    Discovery route{};auto input=pb_istream_from_buffer(payload,payloadSize);
    if(!pb_decode(&input,meshtastic_RouteDiscovery_fields,&route)||input.bytes_left||route.route_count>7||
       route.snr_towards_count>route.route_count||route.route_back_count||route.snr_back_count)return false;
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TRACEROUTE_APP;
    data.payload.size=payloadSize;if(payloadSize)memcpy(data.payload.bytes,payload,payloadSize);
    data.want_response=true;data.has_bitfield=true;data.bitfield=0;
    auto stream=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&stream,meshtastic_Data_fields,&data))return false;
    size=stream.bytes_written;return size<=239;
  }
  static bool responseData(const uint8_t *payload,size_t payloadSize,uint32_t request,
                           uint8_t *out,size_t capacity,size_t &size) {
    size=0;if(!out||!request||payloadSize>MaxPayload||(payloadSize&&!payload))return false;
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TRACEROUTE_APP;
    data.payload.size=payloadSize;if(payloadSize)memcpy(data.payload.bytes,payload,payloadSize);
    data.request_id=request;data.has_bitfield=true;data.bitfield=0;
    auto stream=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&stream,meshtastic_Data_fields,&data))return false;
    size=stream.bytes_written;return size<=239;
  }
};
}
