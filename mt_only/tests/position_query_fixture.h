// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "endpoint_fixture.h"
#include <string>
static meshtastic_ToRadio query(uint32_t id=77,uint32_t to=PEER,unsigned hop=3) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.id=id;p.to=to;p.hop_limit=hop;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_POSITION_APP;p.decoded.want_response=true;return r;
}
static meshtastic_Position fullPosition() {
  meshtastic_Position p{};p.has_latitude_i=p.has_longitude_i=p.has_altitude=true;
  p.latitude_i=123456789;p.longitude_i=-987654321;p.altitude=-35;p.time=1700000000;
  p.location_source=meshtastic_Position_LocSource_LOC_EXTERNAL;p.altitude_source=meshtastic_Position_AltSource_ALT_BAROMETRIC;
  p.timestamp=1699999999;p.timestamp_millis_adjust=-123;p.has_altitude_hae=true;p.altitude_hae=-10;
  p.has_altitude_geoidal_separation=true;p.altitude_geoidal_separation=-25;
  p.PDOP=135;p.HDOP=80;p.VDOP=109;p.gps_accuracy=3000;p.has_ground_speed=p.has_ground_track=true;
  p.ground_speed=42;p.ground_track=12345;p.fix_quality=1;p.fix_type=3;p.sats_in_view=11;
  p.sensor_id=2;p.next_update=3600;p.seq_number=7;p.precision_bits=24;return p;
}
static std::vector<uint8_t> encodePosition(const meshtastic_Position &p) {
  uint8_t b[233]{};auto o=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&o,meshtastic_Position_fields,&p));return {b,b+o.bytes_written};
}
static std::vector<uint8_t> encodeData(const std::vector<uint8_t> &payload,uint32_t request=77,uint32_t port=3,bool want=false) {
  meshtastic_Data d{};d.portnum=static_cast<meshtastic_PortNum>(port);d.request_id=request;d.has_bitfield=true;d.want_response=want;
  d.payload.size=payload.size();assert(payload.size()<=sizeof(d.payload.bytes));if(!payload.empty())memcpy(d.payload.bytes,payload.data(),payload.size());
  uint8_t b[239]{};auto o=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&o,meshtastic_Data_fields,&d));return {b,b+o.bytes_written};
}
static std::vector<uint8_t> rawFrame(const std::vector<uint8_t> &data,uint32_t from=PEER,uint32_t to=OURS,uint32_t id=700,uint8_t flags=0) {
  assert(data.size()<=239);std::vector<uint8_t> f(16+data.size());
  for(unsigned i=0;i<4;++i){f[i]=uint8_t(to>>(i*8));f[i+4]=uint8_t(from>>(i*8));f[i+8]=uint8_t(id>>(i*8));}
  f[12]=flags;f[13]=8;f[15]=uint8_t(from);if(!data.empty())memcpy(f.data()+16,data.data(),data.size());
  uint8_t nonce[16]{};bench::MtCodec::nonce(from,id,nonce);assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,f.data()+16,data.size()));return f;
}
static unsigned positions(const Host &h,uint32_t request=77,std::vector<uint8_t> *payload=nullptr) {
  unsigned count=0;for(const auto &o:h.out)if(o.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    o.packet.decoded.portnum==meshtastic_PortNum_POSITION_APP&&o.packet.decoded.request_id==request) {
    ++count;if(payload)*payload={o.packet.decoded.payload.bytes,o.packet.decoded.payload.bytes+o.packet.decoded.payload.size};
  }
  return count;
}
static void beginQuery(Fixture &f,uint32_t now=100,uint8_t owner=0) {
  auto q=query();input(owner?f.bc:f.c,q,now);f.s.poll(now+200,0);assert(f.radio.sends==1);f.done(now+201);
}
static std::vector<uint8_t> goodFrame(uint32_t request=77,uint32_t id=700) {return rawFrame(encodeData(encodePosition(fullPosition()),request),PEER,OURS,id);}
