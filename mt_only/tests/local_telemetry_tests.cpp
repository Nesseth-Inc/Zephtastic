// SPDX-License-Identifier: GPL-3.0-only
#include "telemetry_query_fixture.h"
static void feedUsb(mt::Companion &c,const uint8_t *bytes,size_t size,uint32_t now) {
 const mt::Companion::WorkspaceProof proof{true};constexpr uint32_t epoch=1;
 c.byte(0x94,now,epoch,proof);c.byte(0xc3,now,epoch,proof);
 c.byte(uint8_t(size>>8),now,epoch,proof);c.byte(uint8_t(size),now,epoch,proof);
 for(size_t i=0;i<size;++i)c.byte(bytes[i],now,epoch,proof);
}
static void feed(Fixture &f,bool ble,const meshtastic_ToRadio &r) {
  if(ble){input(f.bc,r,0xfffffff0);return;}
  uint8_t wire[512];auto out=pb_ostream_from_buffer(wire,sizeof(wire));assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
  feedUsb(f.c,wire,out.bytes_written,123);
}
int main(){
 for(uint8_t model:{81,127})for(bool ble:{false,true}) {
  auto f=std::make_unique<Fixture>(false,model);f->s.stop(); // no armed/running radio required
  auto &h=ble?f->bh:f->h;auto &other=ble?f->h:f->bh;
  auto q=query(77,OURS,0);q.packet.from=PEER;q.packet.want_ack=true;
  feed(*f,ble,q);assert(results(h,77,meshtastic_Routing_Error_NO_INTERFACE)==1&&!telemetries(h));h.out.clear();
  for(uint32_t seconds:{0u,4294967u,4294968u,UINT32_MAX}) {
   h.uptimeValid=true;h.uptime=seconds;feed(*f,ble,q);std::vector<uint8_t> bytes;
   assert(telemetries(h,77,&bytes)==1&&!results(h,77));meshtastic_Telemetry t{};auto in=pb_istream_from_buffer(bytes.data(),bytes.size());assert(pb_decode(&in,meshtastic_Telemetry_fields,&t));
   const auto &m=t.variant.device_metrics;assert(t.which_variant==meshtastic_Telemetry_device_metrics_tag&&!t.time&&m.has_uptime_seconds&&m.uptime_seconds==seconds);
   assert(!m.has_battery_level&&!m.has_voltage&&!m.has_air_util_tx&&
          m.has_channel_utilization&&m.channel_utilization==0.0f);
   for(const auto &r:h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag){assert(r.packet.from==OURS&&r.packet.to==OURS&&!r.packet.want_ack&&!r.packet.pki_encrypted&&!r.packet.public_key.size&&!r.packet.decoded.want_response&&!r.packet.hop_limit);}
   assert(other.out.empty()&&!f->radio.sends&&!f->s.endpointArmed()&&!f->s.receiving());h.out.clear();
  }
  auto warm=fullTelemetry();warm.time=0;auto bytes=encodeTelemetry(warm);auto cached=q;cached.packet.decoded.payload.size=bytes.size();memcpy(cached.packet.decoded.payload.bytes,bytes.data(),bytes.size());
  feed(*f,ble,cached);assert(telemetries(h)==1);h.out.clear();
  for(unsigned mode=0;mode<9;++mode){auto bad=q;
   switch(mode){case 0:bad.packet.id=0;break;case 1:bad.packet.decoded.want_response=false;break;case 2:bad.packet.decoded.request_id=9;break;case 3:bad.packet.decoded.reply_id=9;break;case 4:bad.packet.decoded.has_bitfield=true;break;case 5:bad.packet.decoded.payload.size=1;bad.packet.decoded.payload.bytes[0]=255;break;case 6:bad.packet.pki_encrypted=true;break;case 7:bad.packet.decoded.payload.size=2;bad.packet.decoded.payload.bytes[0]=0x1a;bad.packet.decoded.payload.bytes[1]=0;break;case 8:bad.packet.channel=1;break;}
   feed(*f,ble,bad);assert(!telemetries(h,bad.packet.id)&&results(h,bad.packet.id)==1&&!f->radio.sends);h.out.clear();
  }
  // A remote query retains the armed-radio gate and cannot read local uptime.
  feed(*f,ble,query());assert(!telemetries(h)&&results(h,77,meshtastic_Routing_Error_NO_INTERFACE)==1&&!f->radio.sends);
  group("local uptime: board/USB-or-BLE, cold/warm cache, zero/max/wrap-independent seconds, correlation, missing metrics, errors, no RF");
 }
 auto remote=std::make_unique<Fixture>();beginQuery(*remote);remote->rx(goodFrame());remote->clients();assert(telemetries(remote->h)==1&&remote->radio.sends==1);
 group("remote query and correlated response remain functional in mock radio");
}
