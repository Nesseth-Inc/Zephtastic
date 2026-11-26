// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "meshtastic/mesh.pb.h"
#include <pb_encode.h>
#include "NeighborInfoPayload.h"
#include "MtCodec.h"

namespace mt {
// Wire encoding only: callers own snapshot validity, disclosure and correlation.
class NeighborQuery {
public:
  static bool response(uint32_t request,const meshtastic_NeighborInfo &info,
                       uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!request||!out)return false;
    meshtastic_Data data{};
    auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(!pb_encode(&payload,meshtastic_NeighborInfo_fields,&info))return false;
    data.payload.size=payload.bytes_written;
    data.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;
    data.request_id=request;
    data.has_bitfield=true;
    auto wire=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&wire,meshtastic_Data_fields,&data))return false;
    size=wire.bytes_written;
    return true;
  }
static bool requestShape(const bench::RouteMetadata &m) {
    return m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP
        && m.payloadLength==0
        && m.wantResponse
        && m.hasBitfield
        && m.bitfield==2
        && m.request_id==0
        && m.reply_id==0
        && m.emoji==0
        && m.signatureLength==0;
  }

  static bool request(uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!out)return false;
    meshtastic_Data data{};
    data.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;
    data.payload.size=0;
    data.want_response=true;
    data.has_bitfield=true;
    data.bitfield=2;
    uint8_t scratch[64];
    auto scratch_stream=pb_ostream_from_buffer(scratch,sizeof(scratch));
    if(!pb_encode(&scratch_stream,meshtastic_Data_fields,&data))return false;
    const size_t written=scratch_stream.bytes_written;
    if(capacity<written)return false;
    for(size_t i=0;i<written;i++)out[i]=scratch[i];
    size=written;
    return true;
  }
  // Shape/encoding only. Callers compare the whole original Data and enforce
  // the authenticated or channel-bound request/response correlation separately.
  static bool responseShape(const bench::RouteMetadata &m) {
    return m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&m.request_id&&
      m.hasBitfield&&m.bitfield<=1&&!m.wantResponse&&!m.reply_id&&!m.emoji&&
      !m.signatureLength&&m.payloadLength&&m.payloadLength<=sizeof(m.payload);
  }
  static bool response(const bench::RouteMetadata &m,uint8_t *out,
                       size_t capacity,size_t &size) {
    size=0;if(!out||!responseShape(m))return false;
    meshtastic_NeighborInfo info{};
    if(!NeighborInfoPayload::decode(m.payload,m.payloadLength,info))return false;
    meshtastic_Data data{};
    auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
    if(!pb_encode(&payload,meshtastic_NeighborInfo_fields,&info))return false;
    data.payload.size=payload.bytes_written;data.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;
    data.request_id=m.request_id;data.has_bitfield=true;data.bitfield=m.bitfield;
    auto wire=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&wire,meshtastic_Data_fields,&data))return false;
    size=wire.bytes_written;return true;
  }
};
}
