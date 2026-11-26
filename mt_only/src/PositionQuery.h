// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtCodec.h"
#include "Config.h"
#include "PositionPrecision.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
namespace mt {
// Public position queries disclose no local coordinates. Received coordinates
// are untrusted peer claims; they never set local position, time or storage.
class PositionQuery {
  struct Wipe {void *p;size_t n;~Wipe(){volatile uint8_t *v=static_cast<volatile uint8_t *>(p);while(n--)*v++=0;}};
  template<typename Enum> static uint32_t enumNumber(const Enum &value) {
    static_assert(sizeof(Enum)==1||sizeof(Enum)==2||sizeof(Enum)==4);
    if constexpr(sizeof(Enum)==1){uint8_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
    else if constexpr(sizeof(Enum)==2){uint16_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
    else {uint32_t raw;memcpy(&raw,&value,sizeof(raw));return raw;}
  }
public:
  static int32_t publicCoordinate(int32_t coordinate) {
    return PositionPrecision::truncateCoordinate(coordinate,Config::PublicPositionPrecision);
  }
  static bool localPosition(const Config &cfg,const meshtastic_Position *live,meshtastic_Position &out) {
    out={};if(!cfg.valid())return false;
    if(cfg.fixedPosition()) {
      out.has_latitude_i=out.has_longitude_i=true;out.latitude_i=cfg.latitudeI();out.longitude_i=cfg.longitudeI();
      out.has_altitude=cfg.fixedAltitude();out.altitude=cfg.altitude();out.location_source=meshtastic_Position_LocSource_LOC_MANUAL;return true;
    }
    if(!cfg.gnssSettings().enabled||!live||!live->has_latitude_i||!live->has_longitude_i||
       live->latitude_i < -900000000||live->latitude_i>900000000||live->longitude_i < -1800000000||live->longitude_i>1800000000)return false;
    out=*live;return true;
  }
  static bool fixedResponse(const Config &cfg,uint32_t request,uint8_t *data,size_t capacity,size_t &size,unsigned channel=0,
                            const meshtastic_Position *live=nullptr,bool broadcast=false,bool wantReplies=false) {
    size=0;if((!request&&!broadcast)||!cfg.valid()||!cfg.positionSharing())return false;
    const unsigned precision=cfg.positionPrecision(channel);if(!precision)return false;
    meshtastic_Position p{};if(!localPosition(cfg,live,p))return false;
    PositionPrecision::apply(p,precision);
    // A geographic-edge cell may have an invalid center: withhold the reply.
    if(p.latitude_i < -900000000||p.latitude_i>900000000||p.longitude_i < -1800000000||p.longitude_i>1800000000)return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_POSITION_APP;d.request_id=request;d.want_response=broadcast&&wantReplies;d.has_bitfield=true;
    auto payload=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));
    if(!pb_encode(&payload,meshtastic_Position_fields,&p))return false;
    d.payload.size=payload.bytes_written;auto out=pb_ostream_from_buffer(data,capacity);
    if(!pb_encode(&out,meshtastic_Data_fields,&d))return false;
    size=out.bytes_written;return true;
  }
  static bool canonicalPositionPayload(const uint8_t *payload,size_t payloadLength) {
    if(!payload||!payloadLength||payloadLength>233)return false;
    meshtastic_Position p{};Wipe pw{&p,sizeof(p)};auto in=pb_istream_from_buffer(payload,payloadLength);
    if(!pb_decode(&in,meshtastic_Position_fields,&p)||in.bytes_left||
      !p.has_latitude_i||!p.has_longitude_i||p.latitude_i < -900000000||p.latitude_i>900000000||
      p.longitude_i < -1800000000||p.longitude_i>1800000000||p.precision_bits>32||
      enumNumber(p.location_source)>3||enumNumber(p.altitude_source)>4)return false;
    uint8_t canonical[233]{};Wipe cw{canonical,sizeof(canonical)};auto out=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&out,meshtastic_Position_fields,&p)&&out.bytes_written==payloadLength&&
      !memcmp(canonical,payload,payloadLength);
  }
  // Only the local custom-channel announcePosition producer uses this shape.
  // It is intentionally distinct from request/reply and rejects all extra Data fields.
  static bool canonicalBroadcast(const uint8_t *data,size_t size) {
    if(!data||!size||size>239)return false;
    meshtastic_Data d{};Wipe dw{&d,sizeof(d)};auto in=pb_istream_from_buffer(data,size);
    if(!pb_decode(&in,meshtastic_Data_fields,&d)||in.bytes_left||
       d.portnum!=meshtastic_PortNum_POSITION_APP||d.want_response||d.dest||d.source||
       d.request_id||d.reply_id||d.emoji||!d.has_bitfield||d.bitfield||d.xeddsa_signature.size||
       !canonicalPositionPayload(d.payload.bytes,d.payload.size))return false;
    uint8_t canonical[239]{};Wipe cw{canonical,sizeof(canonical)};auto out=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&out,meshtastic_Data_fields,&d)&&out.bytes_written==size&&!memcmp(canonical,data,size);
  }
  static bool responseShape(const bench::RouteMetadata &m) {
    return m.portnum==meshtastic_PortNum_POSITION_APP&&m.request_id&&!m.wantResponse&&
      !m.reply_id&&!m.emoji&&!m.signatureLength&&m.hasBitfield&&!(m.bitfield&~1u);
  }
  static bool negative(const bench::RouteMetadata &m,uint8_t *data,size_t capacity,size_t &size) {
    size=0;if(m.portnum!=meshtastic_PortNum_ROUTING_APP||!m.request_id||!m.routingPresent||
      !m.routingErrorPresent||!m.routingError||m.wantResponse||m.reply_id||m.emoji||m.signatureLength||
      !m.hasBitfield||(m.bitfield&~1u)||m.routingError>0x7fffffffu)return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_ROUTING_APP;d.request_id=m.request_id;d.has_bitfield=true;d.bitfield=m.bitfield;
    // Do not narrow unknown wire errors into -fshort-enums. Canonical positive
    // int32 enum values can exceed the target enum width and still mean NAK.
    d.payload.bytes[0]=24;d.payload.size=1;uint32_t value=m.routingError;
    do {d.payload.bytes[d.payload.size++]=uint8_t((value&127u)|(value>127u?128u:0u));value>>=7;}while(value);
    auto wire=pb_ostream_from_buffer(data,capacity);
    if(!pb_encode(&wire,meshtastic_Data_fields,&d))return false;
    size=wire.bytes_written;return true;
  }
  static bool response(const bench::RouteMetadata &m,uint8_t *data,size_t capacity,size_t &size) {
    size=0;if(!responseShape(m)||!m.payloadLength)return false;
    if(!canonicalPositionPayload(m.payload,m.payloadLength))return false;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_POSITION_APP;d.request_id=m.request_id;
    d.has_bitfield=true;d.bitfield=m.bitfield;d.payload.size=m.payloadLength;memcpy(d.payload.bytes,m.payload,m.payloadLength);
    auto wire=pb_ostream_from_buffer(data,capacity);
    if(!pb_encode(&wire,meshtastic_Data_fields,&d))return false;
    size=wire.bytes_written;return true;
  }
};
}
