// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
static void feedUsb(mt::Companion &c,const uint8_t *bytes,size_t size,uint32_t now) {
 const mt::Companion::WorkspaceProof proof{true};constexpr uint32_t epoch=1;
 c.byte(0x94,now,epoch,proof);c.byte(0xc3,now,epoch,proof);
 c.byte(uint8_t(size>>8),now,epoch,proof);c.byte(uint8_t(size),now,epoch,proof);
 for(size_t i=0;i<size;++i)c.byte(bytes[i],now,epoch,proof);
}
static void feed(Fixture &f,bool ble,const meshtastic_ToRadio &q,bool unknown=false){
 uint8_t bytes[512];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes)-3);assert(pb_encode(&out,meshtastic_ToRadio_fields,&q));size_t n=out.bytes_written;
 if(unknown){bytes[n++]=0xf8;bytes[n++]=0x07;bytes[n++]=1;}
 if(ble){f.bc.packet(bytes,n,123,mt::Companion::WorkspaceProof{true});return;}
 feedUsb(f.c,bytes,n,123);
}
static unsigned replies(const Host &h,uint32_t port,meshtastic_MeshPacket *packet=nullptr){unsigned count=0;
 for(const auto &o:h.out)if(o.which_payload_variant==meshtastic_FromRadio_packet_tag&&uint32_t(o.packet.decoded.portnum)==port&&o.packet.decoded.request_id==77){++count;if(packet)*packet=o.packet;}
 return count;
}
int main(int argc,char **argv){
 if(argc==4){
  auto f=std::make_unique<Fixture>(false,uint8_t(strtoul(argv[1],nullptr,10)));f->s.stop();auto cfg=f->s.config;assert(cfg.setFixedPosition(123456789,-456789012,true,-12));cfg.setPositionSharing(false);assert(f->s.apply(cfg));
  const bool ble=strcmp(argv[2],"ble")==0;auto &h=ble?f->bh:f->h;const size_t n=strlen(argv[3])/2;assert(n<512&&strlen(argv[3])==n*2);uint8_t bytes[512];
  for(size_t i=0;i<n;++i){char hex[3]={argv[3][2*i],argv[3][2*i+1],0};bytes[i]=uint8_t(strtoul(hex,nullptr,16));}
  if(ble)f->bc.packet(bytes,n,123,mt::Companion::WorkspaceProof{true});else feedUsb(f->c,bytes,n,123);
  for(const auto &r:h.out){uint8_t out[512];auto stream=pb_ostream_from_buffer(out,sizeof(out));assert(pb_encode(&stream,meshtastic_FromRadio_fields,&r));std::cout<<"OUT ";for(size_t i=0;i<stream.bytes_written;++i)printf("%02x",out[i]);std::cout<<"\n";}
  assert(!f->radio.sends&&!(ble?f->h:f->bh).out.size()&&!f->s.config.positionSharing());return 0;
 }

 for(uint8_t model:{81,127})for(bool ble:{false,true})for(auto port:{meshtastic_PortNum_NODEINFO_APP,meshtastic_PortNum_POSITION_APP}){
  auto f=std::make_unique<Fixture>(false,model);f->s.stop();auto &h=ble?f->bh:f->h;auto &other=ble?f->h:f->bh;
  meshtastic_ToRadio q{};q.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=q.packet;p.to=OURS;p.from=PEER;p.id=77;p.want_ack=true;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=port;p.decoded.want_response=true;
  if(port==meshtastic_PortNum_POSITION_APP){feed(*f,ble,q);assert(results(h,77,meshtastic_Routing_Error_NO_RESPONSE)==1&&!replies(h,port));h.out.clear();}
  for(bool sharing:{false,true})for(bool altitude:{false,true}){
   auto cfg=f->s.config;assert(cfg.setFixedPosition(123456789,-456789012,altitude,-12));cfg.setPositionSharing(sharing);assert(f->s.apply(cfg));auto before=f->s.config;
   feed(*f,ble,q);meshtastic_MeshPacket reply{};assert(replies(h,port,&reply)==1&&!results(h,77));assert(reply.from==OURS&&reply.to==OURS&&!reply.want_ack&&!reply.pki_encrypted&&!reply.public_key.size&&!reply.hop_limit&&!reply.decoded.want_response);
   auto in=pb_istream_from_buffer(reply.decoded.payload.bytes,reply.decoded.payload.size);
   if(port==meshtastic_PortNum_NODEINFO_APP){meshtastic_User value{};assert(pb_decode(&in,meshtastic_User_fields,&value));assert(!strcmp(value.id,"!12345678")&&value.hw_model==model&&value.role==meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE&&!value.public_key.size);}
   else {meshtastic_Position value{};assert(pb_decode(&in,meshtastic_Position_fields,&value));assert(value.has_latitude_i&&value.has_longitude_i&&value.latitude_i==123456789&&value.longitude_i==-456789012&&value.has_altitude==altitude&&(!altitude||value.altitude==-12));assert(!value.time&&!value.timestamp&&!value.precision_bits);}
   assert(f->s.config.same(before)&&other.out.empty()&&!f->radio.sends&&!f->s.receiving()&&!f->s.endpointArmed());h.out.clear();
   // Public response is still independently consent-gated and 15-bit coarsened.
   uint8_t wire[239];size_t size=0;assert(mt::PositionQuery::fixedResponse(cfg,77,wire,sizeof(wire),size)==sharing);
   if(sharing){meshtastic_Data d{};auto ds=pb_istream_from_buffer(wire,size);assert(pb_decode(&ds,meshtastic_Data_fields,&d));meshtastic_Position value{};auto ps=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&ps,meshtastic_Position_fields,&value));assert(value.precision_bits==15&&value.latitude_i==mt::PositionQuery::publicCoordinate(123456789)&&value.longitude_i==mt::PositionQuery::publicCoordinate(-456789012));}
  }
  auto before=f->s.config;
  for(unsigned mode:{0u,1u,2u,3u,4u,5u,7u,8u,9u,10u,11u}){auto bad=q;
   switch(mode){case 0:bad.packet.id=0;break;case 1:bad.packet.decoded.want_response=false;break;case 2:bad.packet.decoded.request_id=1;break;case 3:bad.packet.decoded.reply_id=1;break;case 4:bad.packet.decoded.payload.size=1;bad.packet.decoded.payload.bytes[0]=0;break;case 5:bad.packet.pki_encrypted=true;break;case 7:bad.packet.channel=1;break;case 8:bad.packet.decoded.has_bitfield=true;break;case 9:bad.packet.hop_start=1;break;case 10:bad.packet.decoded.xeddsa_signature.size=1;break;default:break;}
   feed(*f,ble,bad,mode==11);assert(!replies(h,port)&&results(h,bad.packet.id)==1&&f->s.config.same(before)&&!f->radio.sends);h.out.clear();
  }
  for(unsigned keyLength:{1u,32u}){
   Fixture baseline(false,model),hinted(false,model);baseline.s.stop();hinted.s.stop();
   auto cfg=f->s.config;assert(baseline.s.apply(cfg)&&hinted.s.apply(cfg));
   auto ordinary=q;auto withHint=ordinary;
   withHint.packet.public_key.size=keyLength;memset(withHint.packet.public_key.bytes,0xa5,keyLength);
   feed(baseline,ble,ordinary);feed(hinted,ble,withHint);
   meshtastic_MeshPacket a{},b{};assert(replies(ble?baseline.bh:baseline.h,port,&a)==1&&
     replies(ble?hinted.bh:hinted.h,port,&b)==1);
   assert(a.id==b.id&&a.from==b.from&&a.to==b.to&&a.decoded.portnum==b.decoded.portnum&&
     a.decoded.request_id==b.decoded.request_id&&a.decoded.want_response==b.decoded.want_response&&
     a.decoded.payload.size==b.decoded.payload.size&&!memcmp(a.decoded.payload.bytes,b.decoded.payload.bytes,a.decoded.payload.size)&&
     !a.pki_encrypted&&!a.public_key.size&&!b.pki_encrypted&&!b.public_key.size&&
     !baseline.radio.sends&&!hinted.radio.sends&&(ble?baseline.h:baseline.bh).out.empty()&&
     (ble?hinted.h:hinted.bh).out.empty()&&baseline.s.config.same(cfg)&&hinted.s.config.same(cfg));
  }
  // Remote destination stays on the existing armed-radio query path.
  q.packet.to=PEER;q.packet.from=0;q.packet.want_ack=false;feed(*f,ble,q);assert(!replies(h,port)&&results(h,77,meshtastic_Routing_Error_NO_INTERFACE)==1&&!f->radio.sends);
 group("local read: board/transport/port, fixed/missing position, exact readback and consent, 11 invalid cases, bounded key hints preserve USB/BLE channel-local response");
 }
}
