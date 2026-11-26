// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "endpoint_fixture.h"
#include <string>
static meshtastic_ToRadio query(uint32_t id=77,uint32_t to=PEER,unsigned hop=3) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.id=id;p.to=to;p.hop_limit=hop;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;p.decoded.want_response=true;return r;
}
static meshtastic_Telemetry fullTelemetry() {
  meshtastic_Telemetry t{};t.time=1700000000;t.which_variant=meshtastic_Telemetry_device_metrics_tag;
  auto &m=t.variant.device_metrics;m.has_battery_level=m.has_voltage=m.has_channel_utilization=m.has_air_util_tx=m.has_uptime_seconds=true;
  m.battery_level=101;m.voltage=4.2f;m.channel_utilization=12.5f;m.air_util_tx=0.25f;m.uptime_seconds=123456;return t;
}
static std::vector<uint8_t> encodeTelemetry(const meshtastic_Telemetry &t) {
  uint8_t b[233]{};auto o=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&o,meshtastic_Telemetry_fields,&t));return {b,b+o.bytes_written};
}
static std::vector<uint8_t> encodeData(const std::vector<uint8_t> &payload,uint32_t request=77,uint32_t port=67,bool want=false) {
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
static unsigned telemetries(const Host &h,uint32_t request=77,std::vector<uint8_t> *payload=nullptr) {
  unsigned count=0;for(const auto &o:h.out)if(o.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    o.packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&o.packet.decoded.request_id==request) {
    ++count;if(payload)*payload={o.packet.decoded.payload.bytes,o.packet.decoded.payload.bytes+o.packet.decoded.payload.size};
  }
  return count;
}
static void beginQuery(Fixture &f,uint32_t now=100,uint8_t owner=0) {
  auto q=query();input(owner?f.bc:f.c,q,now);f.s.poll(now+200,0);assert(f.radio.sends==1);f.done(now+201);
}
static std::vector<uint8_t> goodFrame(uint32_t request=77,uint32_t id=700) {return rawFrame(encodeData(encodeTelemetry(fullTelemetry()),request),PEER,OURS,id);}
