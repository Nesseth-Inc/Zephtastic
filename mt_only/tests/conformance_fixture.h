// Shared protocol setup copied from baseline endpoint tests; no expectations.
// SPDX-License-Identifier: GPL-3.0-only
#include "Companion.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <vector>
#include <memory>
#include <iostream>
constexpr uint32_t OURS=0x12345678,PEER=0x87654321;
struct Radio:mt::Radio {
  int sends=0,sendStatus=0,restoreStatus=0,healthStatus=1,doneStatus=0;
  uint32_t air=100;bool done=false,occupied=false;uint8_t *pending=nullptr;
  std::vector<std::vector<uint8_t>> frames;
  bool ready()override{return true;}int start()override{return 0;}int stop()override{pending=nullptr;return 0;}
  int restore()override{return restoreStatus;}int health()override{return healthStatus;}
  bool busy()override{return occupied;}uint32_t airtime(size_t)override{return air;}
  int send(uint8_t *p,size_t n)override{++sends;frames.emplace_back(p,p+n);pending=p;done=false;return sendStatus;}
  bool completion(int &result)override{result=doneStatus;return done;}
};
struct Host:mt::Companion::Host {
  mt::Service &s;std::vector<meshtastic_FromRadio> out;
  explicit Host(mt::Service &s):s(s){}
  bool ready()const override{return true;}
  int save(const mt::Config &c)override{s.stop();return s.apply(c)?0:-1;}
  bool send(const uint8_t *p,size_t n)override {
    meshtastic_FromRadio f{};auto b=pb_istream_from_buffer(p+4,n-4);
    assert(pb_decode(&b,meshtastic_FromRadio_fields,&f));out.push_back(f);return true;
  }
};
static void input(mt::Companion &c,const meshtastic_ToRadio &r,uint32_t now) {
  uint8_t bytes[512];auto b=pb_ostream_from_buffer(bytes,512);assert(pb_encode(&b,meshtastic_ToRadio_fields,&r));c.packet(bytes,b.bytes_written,now,mt::Companion::WorkspaceProof{true});
}
struct Fixture {
  mt::test::KnownChannelFixtureClock knownChannelClock;
  Radio radio;mt::Service s;Host h,bh;mt::companion::CompanionWorkspace workspace;mt::Companion c,bc;
  explicit Fixture(bool tx=true,uint8_t model=81):s(radio,tx,OURS,model,true),h(s),bh(s),c(s,h,model,workspace,0),bc(s,bh,model,workspace,1) {
    knownChannelClock.bind(s);
    mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);
    assert(s.apply(cfg)&&s.start(false,0));assert(s.armEndpoint(tx));
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;
    input(c,r,1);input(bc,r,1);h.out.clear();bh.out.clear();
  }
  void text(uint32_t id=1,uint32_t to=PEER,bool ack=true,uint32_t now=100,uint8_t owner=0,unsigned hop=3,const char *message="Hello") {
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=id;p.to=to;p.want_ack=ack;p.hop_limit=hop;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=strlen(message);memcpy(p.decoded.payload.bytes,message,strlen(message));
    input(owner?bc:c,r,now);
  }
  std::vector<uint8_t> wire(uint32_t from,uint32_t to,uint32_t id,uint32_t request=0,uint32_t code=0,uint8_t flags=0,bool text=false,bool bitfield=true) {
    meshtastic_Data d{};d.portnum=text?meshtastic_PortNum_TEXT_MESSAGE_APP:meshtastic_PortNum_ROUTING_APP;d.request_id=request;d.has_bitfield=true;
    if(text){d.payload.size=5;memcpy(d.payload.bytes,"Hello",5);}
    else {meshtastic_Routing r{};r.which_variant=meshtastic_Routing_error_reason_tag;r.error_reason=static_cast<meshtastic_Routing_Error>(code);
      auto b=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&b,meshtastic_Routing_fields,&r));d.payload.size=b.bytes_written;}
    uint8_t bytes[239],frame[255];auto b=pb_ostream_from_buffer(bytes,239);assert(pb_encode(&b,meshtastic_Data_fields,&d));size_t len;
    assert(s.codec.encodeZeroHopData(from,to,id,bytes,b.bytes_written,frame,255,len));frame[12]=flags;
    if(!bitfield){assert(bytes[b.bytes_written-2]==72);len-=2;uint8_t nonce[16];bench::MtCodec::nonce(from,id,nonce);memcpy(frame+16,bytes,b.bytes_written-2);assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,frame+16,b.bytes_written-2));}
    return {frame,frame+len};
  }
  void rx(const std::vector<uint8_t> &f,uint32_t now=500,uint32_t random=700) {
    s.rx(f.data(),f.size(),3,now,random);c.receive(f.data(),f.size(),-90,3,now,mt::Companion::WorkspaceProof{true});bc.receive(f.data(),f.size(),-90,3,now,mt::Companion::WorkspaceProof{true});
  }
  void done(uint32_t now=400){radio.done=true;s.poll(now,0);}
  void clients(){c.poll(mt::Companion::WorkspaceProof{true});bc.poll(mt::Companion::WorkspaceProof{true});}
};
static unsigned results(const Host &h,uint32_t request,int code=-1,uint32_t from=0) {
  unsigned n=0;for(auto &f:h.out)if(f.which_payload_variant==meshtastic_FromRadio_packet_tag&&f.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&f.packet.decoded.request_id==request) {
    meshtastic_Routing r{};auto b=pb_istream_from_buffer(f.packet.decoded.payload.bytes,f.packet.decoded.payload.size);assert(pb_decode(&b,meshtastic_Routing_fields,&r));
    assert(r.which_variant==meshtastic_Routing_error_reason_tag);
    if((code<0||r.error_reason==code)&&(!from||f.packet.from==from))++n;
  }
  return n;
}
