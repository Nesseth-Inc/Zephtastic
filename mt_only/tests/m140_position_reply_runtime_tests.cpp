// SPDX-License-Identifier: GPL-3.0-only
// Role-aware position reply construction; in-memory service/endpoint only.
#include "position_query_fixture.h"
#include "known_channel_fixture.h"
#include "modules/TakPliReplyPayload.h"
#include "pb_decode.h"
#include <cassert>
#include <iostream>

namespace {
unsigned groups=0;
void pass(const char *name){++groups;std::cout<<"PASS "<<name<<"\n";}
std::vector<uint8_t> selector(uint32_t from=PEER,uint32_t id=100){
  return rawFrame({8,3,24,1,72,2},from,OURS,id);
}
void configurePosition(Fixture &f,unsigned role=0,unsigned precision=15,bool share=true){
  f.s.stop();auto c=f.s.config;c.bytes[4]=1;
  assert(c.setFixedPosition(123456789,-987654321,true,-35));c.setPositionSharing(share);
  assert(c.setPrimaryPositionPrecision(precision));
  if(role!=0)assert(c.setDeviceRole(role,0));
  assert(c.valid()&&f.s.apply(c)&&f.s.start(false,0)&&f.s.armEndpoint(true));
}
meshtastic_Data decodeData(const uint8_t *bytes,size_t size){
  meshtastic_Data d{};auto in=pb_istream_from_buffer(bytes,size);
  assert(pb_decode(&in,meshtastic_Data_fields,&d)&&in.bytes_left==0);return d;
}
void checkTakData(const bench::RouteMetadata &m,uint32_t request){
  assert(m.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&m.request_id==request&&
    m.hasBitfield&&m.bitfield==0&&m.payloadLength>1&&m.payload[0]==0xff);
  meshtastic_TAKPacketV2 pli{};auto in=pb_istream_from_buffer(m.payload+1,m.payloadLength-1);
  assert(pb_decode(&in,&meshtastic_TAKPacketV2_msg,&pli)&&in.bytes_left==0);
  assert(pli.latitude_i==mt::PositionQuery::publicCoordinate(123456789)&&
    pli.longitude_i==mt::PositionQuery::publicCoordinate(-987654321)&&pli.altitude==0&&
    pli.course==0&&pli.speed==0&&pli.how==meshtastic_CotHow_CotHow_h_e);
}
struct PrivateEncoder final:mt::Endpoint::ControlEncoder {
  unsigned channel=0,calls=0;
  meshtastic_Team team=meshtastic_Team_Blue;meshtastic_MemberRole role=meshtastic_MemberRole_Medic;
  unsigned channelIndex()const override{return channel;}
  bool encodeChannelControl(uint32_t,uint32_t,const uint8_t*,size_t,bench::Frame&)override{return false;}
  bool positionReplyData(const mt::Config &cfg,const meshtastic_Position &live,unsigned selected,
                         uint32_t request,uint8_t *out,size_t capacity,size_t &size)const override {
    auto *self=const_cast<PrivateEncoder*>(this);++self->calls;size=0;
    if(selected!=channel||!request)return false;
    if(cfg.role()!=10)return mt::PositionQuery::fixedResponse(cfg,request,out,capacity,size,selected,&live);
    meshtastic_Position p{};if(!mt::PositionQuery::localPosition(cfg,&live,p))return false;
    const auto precision=cfg.positionPrecision(selected);if(!precision)return false;
    mt::PositionPrecision::apply(p,precision);
    const auto owner=cfg.node();char name[25]{};memcpy(name,owner.bytes+bench::MtNodeRecord::Name,24);
    const mt::tak_pli::Inputs inputs{&p,name,OURS,0,cfg.fixedPosition(),true,team,role};
    return mt::tak_pli_reply::encode(inputs,request,out,capacity,size)==mt::tak_pli_reply::Result::Ok;
  }
};
}

int main(){
  {
    Fixture f;configurePosition(f);f.rx(selector(),100,700);f.s.poll(400,0);
    assert(f.radio.frames.size()==1);bench::RouteMetadata m;
    assert(f.s.codec.decode(f.radio.frames.back().data(),f.radio.frames.back().size(),m));
    assert(m.portnum==meshtastic_PortNum_POSITION_APP&&m.request_id==100&&m.hasBitfield&&m.bitfield==0);
    meshtastic_Position p{};auto in=pb_istream_from_buffer(m.payload,m.payloadLength);
    assert(pb_decode(&in,meshtastic_Position_fields,&p)&&in.bytes_left==0&&p.has_latitude_i&&p.has_longitude_i);
    pass("ordinary client role retains the correlated port-3 fixed-position reply");
  }
#if defined(CONFIG_MT_TAK_TRACKER)
  {
    Fixture f;configurePosition(f,10,15,true);assert(f.s.takConfiguration(uint32_t(meshtastic_Team_Blue),uint32_t(meshtastic_MemberRole_Medic)));
    f.rx(selector(),100,700);f.s.poll(400,0);assert(f.radio.frames.size()==1);
    bench::RouteMetadata m;assert(f.s.codec.decode(f.radio.frames.back().data(),f.radio.frames.back().size(),m));
    assert(m.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&m.request_id==100&&m.hasBitfield&&m.bitfield==0);
    checkTakData(m,100);
    meshtastic_TAKPacketV2 pli{};auto in=pb_istream_from_buffer(m.payload+1,m.payloadLength-1);
    assert(pb_decode(&in,&meshtastic_TAKPacketV2_msg,&pli));
    assert(pli.team==meshtastic_Team_Blue&&pli.role==meshtastic_MemberRole_Medic&&
      !strcmp(pli.callsign,"ZephTastic")&&!strcmp(pli.device_callsign,"ZephTastic")&&
      !strcmp(pli.uid,"!12345678")&&pli.battery==0&&pli.how==meshtastic_CotHow_CotHow_h_e);
    pass("TAK role returns a default-public, request-correlated Data layer with fixed-position identity and bounded battery");
  }
  {
    Fixture unavailable;configurePosition(unavailable,10);assert(!unavailable.s.takConfiguration(0,0,false));
    unavailable.rx(selector(),100,700);unavailable.s.poll(400,0);assert(unavailable.radio.frames.empty());
    Fixture noPrecision;configurePosition(noPrecision,10,0,true);assert(noPrecision.s.takConfiguration(0,0));
    noPrecision.rx(selector(),100,701);noPrecision.s.poll(400,0);assert(noPrecision.radio.frames.empty());
    Fixture noPosition;noPosition.s.stop();auto c=noPosition.s.config;c.bytes[4]=1;
    auto gnss=c.gnssSettings();gnss.enabled=true;assert(c.setGnssSettings(gnss));c.setPositionSharing(true);
    assert(c.setDeviceRole(10,0)&&noPosition.s.apply(c)&&noPosition.s.start(false,0)&&noPosition.s.armEndpoint(true));
    assert(noPosition.s.takConfiguration(0,0));noPosition.rx(selector(),100,702);noPosition.s.poll(400,0);
    assert(noPosition.radio.frames.empty());
    pass("missing TAK settings, channel precision, or current position refuses before enqueue");
  }
#endif
  {
    auto cfg=mt::Config{};cfg.bytes[4]=1;assert(cfg.setFixedPosition(123456789,-987654321,true,-35));
    cfg.setPositionSharing(true);assert(cfg.setPrimaryPositionPrecision(8));
    PrivateEncoder owner;mt::Endpoint endpoint(&owner);endpoint.positionConfig(cfg);
    uint8_t data[239]{};size_t size=0;
    assert(endpoint.privatePositionReply(PEER,55,100,data,sizeof(data),size)&&size>0&&owner.calls==1);
    auto out=decodeData(data,size);assert(out.portnum==meshtastic_PortNum_POSITION_APP&&out.request_id==55);
    uint8_t tooSmall[1];memset(tooSmall,0xa5,sizeof(tooSmall));size=0;
    assert(!endpoint.privatePositionReply(PEER+1,56,200,tooSmall,sizeof(tooSmall),size)&&!size&&owner.calls==2);
    for(auto b:tooSmall)assert(b==0xa5);
    mt::Endpoint noOwner;noOwner.positionConfig(cfg);size=0;
    assert(noOwner.privatePositionReply(PEER,57,300,data,64,size)&&decodeData(data,size).portnum==meshtastic_PortNum_POSITION_APP);
    pass("private endpoint reply callback delegates bounded Data construction; ordinary role stays on port 3");
  }
#if defined(CONFIG_MT_TAK_TRACKER)
  {
    auto cfg=mt::Config{};cfg.bytes[4]=1;assert(cfg.setFixedPosition(123456789,-987654321,false,0));
    cfg.setPositionSharing(true);assert(cfg.setPrimaryPositionPrecision(8)&&cfg.setDeviceRole(10,0));
    PrivateEncoder owner;mt::Endpoint endpoint(&owner);endpoint.positionConfig(cfg);
    uint8_t reply[239]{};size_t replySize=0;
    assert(endpoint.privatePositionReply(PEER,57,300,reply,sizeof(reply),replySize)&&owner.calls==1);
    const auto response=decodeData(reply,replySize);
    assert(response.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&response.request_id==57&&response.payload.bytes[0]==0xff);
    mt::Endpoint noOwner;noOwner.positionConfig(cfg);uint8_t data[239]{};size_t size=0;
    assert(!noOwner.privatePositionReply(PEER,58,300,data,sizeof(data),size)&&!size);
    pass("role 10 private reply fails closed without an explicit owner encoder");
  }
#else
  {
    auto cfg=mt::Config{};cfg.bytes[4]=1;assert(cfg.setFixedPosition(123456789,-987654321,false,0));
    cfg.setPositionSharing(true);cfg.bytes[107]=10;cfg.bytes[108]=0;cfg.seal();assert(cfg.valid());
    mt::Endpoint noOwner;noOwner.positionConfig(cfg);uint8_t data[64]{};size_t size=0;
    assert(!noOwner.privatePositionReply(PEER,57,300,data,sizeof(data),size)&&!size);
    pass("feature-off stored role 10 cannot fall back to port 3");
  }
#endif
  std::cout<<"PASS "<<groups<<" M140 position-reply runtime groups; no RF hardware used\n";
}
