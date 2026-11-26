// SPDX-License-Identifier: GPL-3.0-only
// Real Service/Endpoint/Companion path; in-memory radio only.
#include "position_query_fixture.h"
#include "modules/TakPliReplyPayload.h"
#include "modules/TakPliReplyValidation.h"
#include <cassert>
#include <iostream>

namespace {
std::vector<uint8_t> stockPliData(uint32_t request,bool malformed=false){
  meshtastic_Position p{};p.latitude_i=123456789;p.longitude_i=-987654321;p.altitude_hae=-25;
  p.ground_speed=42;p.ground_track=12345000;p.location_source=meshtastic_Position_LocSource_LOC_INTERNAL;
  const mt::tak_pli::Inputs inputs{&p,"Peer Callsign",0x87654321,72,false,true,
    meshtastic_Team_Green,meshtastic_MemberRole_Medic};
  uint8_t payload[233]{};size_t payloadSize=0;
  assert(mt::tak_pli::encode(inputs,payload,sizeof(payload),payloadSize)==mt::tak_pli::Result::Ok);
  if(malformed){assert(payloadSize+3<=sizeof(payload));payload[payloadSize++]=0x98;payload[payloadSize++]=0x06;payload[payloadSize++]=1;}
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_ATAK_PLUGIN_V2;d.request_id=request;d.has_bitfield=true;
  d.payload.size=payloadSize;memcpy(d.payload.bytes,payload,payloadSize);
  uint8_t wire[239]{};auto out=pb_ostream_from_buffer(wire,sizeof(wire));assert(pb_encode(&out,meshtastic_Data_fields,&d));
  return {wire,wire+out.bytes_written};
}
std::vector<uint8_t> responseFrame(uint32_t request=77,uint32_t packet=700,uint32_t from=PEER,uint32_t to=OURS,
                                   bool malformed=false,uint8_t channel=8,bool reorder=false){
  auto wire=stockPliData(request,malformed);
  if(reorder){
    // Reorder the canonical Data fields without changing their values/length.
    // MtCodec can parse it, but authenticated-query validation must compare the
    // re-encrypted bytes to the canonical reconstruction and reject it.
    std::vector<std::vector<uint8_t>> fields;size_t at=0;
    while(at<wire.size()){
      const size_t begin=at;uint64_t key=0;unsigned shift=0;
      do{assert(at<wire.size()&&shift<64);const uint8_t b=wire[at++];key|=uint64_t(b&127u)<<shift;shift+=7;if(!(b&128u))break;}while(true);
      const unsigned kind=unsigned(key&7u);
      if(kind==0){do{assert(at<wire.size());if(!(wire[at++]&128u))break;}while(true);}
      else if(kind==1){assert(wire.size()-at>=8);at+=8;}
      else if(kind==2){uint64_t n=0;shift=0;do{assert(at<wire.size()&&shift<64);const uint8_t b=wire[at++];n|=uint64_t(b&127u)<<shift;shift+=7;if(!(b&128u))break;}while(true);assert(n<=wire.size()-at);at+=size_t(n);}
      else if(kind==5){assert(wire.size()-at>=4);at+=4;}else assert(false);
      fields.emplace_back(wire.begin()+begin,wire.begin()+at);
    }
    assert(fields.size()>=3);std::swap(fields[0],fields[1]);wire.clear();for(const auto &field:fields)wire.insert(wire.end(),field.begin(),field.end());
  }
  auto frame=rawFrame(wire,from,to,packet);frame[13]=channel;return frame;
}
void configure(Fixture &f,unsigned role,bool sharing=true){
  f.s.stop();auto cfg=f.s.config;cfg.bytes[4]=1;
  assert(cfg.setDeviceRole(role,0)&&cfg.setFixedPosition(400000000,-800000000,true,60));
  cfg.setPositionSharing(sharing);assert(cfg.setPrimaryPositionPrecision(15));
  assert(cfg.valid()&&f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
#if defined(CONFIG_MT_TAK_TRACKER)
  if(role==10)assert(f.s.takConfiguration(uint32_t(meshtastic_Team_Cyan),uint32_t(meshtastic_MemberRole_TeamMember)));
#else
  assert(role!=10);
#endif
}
void startQuery(Fixture &f,uint8_t owner=0,uint32_t request=77){
  auto q=query(request);input(owner?f.bc:f.c,q,100);f.s.poll(300,0);assert(f.radio.sends==1);f.done(301);
}
unsigned replies(const Host &h,uint32_t request=77){
  unsigned n=0;
  for(const auto &o:h.out){
    if(o.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       o.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
       o.packet.decoded.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&
       o.packet.decoded.request_id==request)++n;
  }
  return n;
}
void feed(Fixture &f,const std::vector<uint8_t>&frame,uint32_t now){f.rx(frame,now,900+now);}
void matchedSingleOwnerAndStrictProof(){
#if defined(CONFIG_MT_TAK_TRACKER)
  Fixture f;configure(f,10);startQuery(f,0,77);
  const auto malformed=responseFrame(77,701,PEER,OURS,true);feed(f,malformed,500);
  const auto wrongPeer=responseFrame(77,702,PEER+1,OURS);feed(f,wrongPeer,600);
  const auto wrongRequest=responseFrame(78,703,PEER,OURS);feed(f,wrongRequest,700);
  const auto wrongTo=responseFrame(77,704,PEER,OURS+1);feed(f,wrongTo,800);
  const auto wrongChannel=responseFrame(77,705,PEER,OURS,false,7);feed(f,wrongChannel,900);
  const auto reordered=responseFrame(77,706,PEER,OURS,false,8,true);feed(f,reordered,1000);
  assert(replies(f.h)==0&&replies(f.bh)==0);
  const auto good=responseFrame(77,707);feed(f,good,1100);
  assert(replies(f.h)==1&&replies(f.bh)==0);
  // Canonical same-content replay is consumed once per observer and owner.
  feed(f,good,1200);assert(replies(f.h)==1&&replies(f.bh)==0);
  std::cout<<"PASS USB-owned position query accepts only matching authenticated-channel canonical port-78 response; malformed/wrong peer/request/to/channel/reordered ciphertext do not complete it and BLE owner receives none\n";
#endif
}
void globalRoleTenThrottleAndOrdinaryBehavior(){
#if defined(CONFIG_MT_TAK_TRACKER)
  Fixture tak;configure(tak,10);
  const auto query1=rawFrame({8,3,24,1,72,2},PEER,OURS,901);feed(tak,query1,100);tak.s.poll(450,0);
  assert(tak.radio.sends==1);tak.done(451);
  const auto query2=rawFrame({8,3,24,1,72,2},PEER+1,OURS,902);feed(tak,query2,5100);tak.s.poll(5450,0);assert(tak.radio.sends==1);
  const auto query3=rawFrame({8,3,24,1,72,2},PEER+2,OURS,903);feed(tak,query3,180100);tak.s.poll(180450,0);assert(tak.radio.sends==2);
  bench::RouteMetadata m;assert(tak.s.codec.decode(tak.radio.frames.back().data(),tak.radio.frames.back().size(),m));assert(m.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&m.request_id==903);
  std::cout<<"PASS role-10 global position reply window suppresses cross-peer retry until the exact 180-second boundary\n";
#endif
  Fixture ordinary;configure(ordinary,0);
  const auto q1=rawFrame({8,3,24,1,72,2},PEER,OURS,911);feed(ordinary,q1,100);ordinary.s.poll(450,0);assert(ordinary.radio.sends==1);ordinary.done(451);
  const auto q2=rawFrame({8,3,24,1,72,2},PEER+1,OURS,912);feed(ordinary,q2,5100);ordinary.s.poll(5450,0);assert(ordinary.radio.sends==2);
  bench::RouteMetadata normal;assert(ordinary.s.codec.decode(ordinary.radio.frames.back().data(),ordinary.radio.frames.back().size(),normal));assert(normal.portnum==meshtastic_PortNum_POSITION_APP);
  std::cout<<"PASS ordinary role retains cross-peer 5-second public position reply behavior\n";
}
}
int main(){matchedSingleOwnerAndStrictProof();globalRoleTenThrottleAndOrdinaryBehavior();}
