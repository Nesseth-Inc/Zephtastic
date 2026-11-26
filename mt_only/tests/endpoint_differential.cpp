// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include <algorithm>

// The included bodies are exact, hash-checked stock functions. Stubs supply
// only the surrounding environment and capture ACK emission; they make no
// ACK decision. This oracle does not exercise stock PKI or route learning.
namespace stock {
using NodeNum=uint32_t;
using PacketId=uint32_t;
constexpr uint32_t NODENUM_BROADCAST=0xffffffffu,NODENUM_BROADCAST_NO_LORA=0xfffffffeu;
constexpr uint8_t NO_NEXT_HOP_PREFERENCE=0;
struct NodeDB {
  struct Node {struct {size_t size=0;} public_key;};
  NodeNum getNodeNum()const{return OURS;}
  uint8_t getLastByteOfNodeNum(NodeNum n)const{return uint8_t(n);}
  Node *getMeshNode(NodeNum){return nullptr;}
} db;
NodeDB *nodeDB=&db;
struct {struct {uint8_t hop_limit=3;} lora;} config;
struct {struct {size_t size=0;} public_key;} owner;
struct Channels {unsigned getPrimaryIndex(){return 0;}} channels;
struct NodeInfoModule {void sendOurNodeInfo(NodeNum,bool,unsigned,bool){}} nodeInfo;
NodeInfoModule *nodeInfoModule=&nodeInfo;
namespace Default {static uint8_t getConfiguredOrDefaultHopLimit(uint8_t n){return n?n:3;}}
namespace Time {static uint32_t stampMillis(){return 0;}}
struct MeshModule {inline static const meshtastic_MeshPacket *currentReply=nullptr;};
int8_t getHopsAway(const meshtastic_MeshPacket &,int8_t defaultIfUnknown=-1);
struct RoutingModule {uint8_t getHopLimitForResponse(const meshtastic_MeshPacket &);};
RoutingModule routing;
RoutingModule *routingModule=&routing;
struct FloodingRouter {void sniffReceived(const meshtastic_MeshPacket *,const meshtastic_Routing *){}};
struct NextHopRouter {void sniffReceived(const meshtastic_MeshPacket *,const meshtastic_Routing *){}};
struct Ack {meshtastic_Routing_Error code;NodeNum to;PacketId request;uint8_t hop;bool reliable;};
struct ReliableRouter:NextHopRouter,FloodingRouter {
  struct {} lastAckProof;
  NodeNum getNodeNum()const{return OURS;}
  std::vector<Ack> emitted;
  bool shouldSuccessAckWithWantAck(const meshtastic_MeshPacket *);
  void sniffReceived(const meshtastic_MeshPacket *,const meshtastic_Routing *);
  bool ackProofPermitsAction(const meshtastic_MeshPacket *,PacketId,bool){return true;}
  void stopRetransmission(NodeNum,PacketId){}
  void noteRouteSuccess(NodeNum,uint32_t){}
  void sendAckNak(meshtastic_Routing_Error code,NodeNum to,PacketId request,unsigned,uint8_t hop,bool reliable=false) {
    emitted.push_back({code,to,request,hop,reliable});
  }
};
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define IS_ONE_OF(value,a,b) ((value)==(a)||(value)==(b))
#define USERPREFS_EVENT_MODE 0
#include "upstream-endpoint.inc"
#undef LOG_INFO
#undef LOG_DEBUG
#undef IS_ONE_OF
#undef USERPREFS_EVENT_MODE
}

static meshtastic_MeshPacket packet(uint32_t id,unsigned port,unsigned start,unsigned hop,bool ack,uint8_t next) {
  meshtastic_MeshPacket p{};p.from=PEER;p.to=OURS;p.id=id;p.hop_start=start;p.hop_limit=hop;
  p.want_ack=ack;p.next_hop=next;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=static_cast<meshtastic_PortNum>(port);p.decoded.has_bitfield=true;
  if(port==1){p.decoded.payload.size=5;memcpy(p.decoded.payload.bytes,"Hello",5);}
  if(port==4) {
    meshtastic_User user{};strcpy(user.id,"!87654321");strcpy(user.long_name,"Peer");strcpy(user.short_name,"PEER");
    auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
    assert(pb_encode(&s,meshtastic_User_fields,&user));p.decoded.payload.size=s.bytes_written;
  }
  return p;
}
static std::vector<uint8_t> encode(Fixture &f,const meshtastic_MeshPacket &p) {
  uint8_t data[239],frame[255];size_t len=0;
  auto s=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&s,meshtastic_Data_fields,&p.decoded));
  assert(f.s.codec.encodeZeroHopData(p.from,p.to,p.id,data,s.bytes_written,frame,sizeof(frame),len));
  frame[12]=uint8_t(p.hop_limit|(p.hop_start<<5)|(p.want_ack?8:0));frame[14]=p.next_hop;frame[15]=p.relay_node;
  return {frame,frame+len};
}
static void expect(Fixture &f,const stock::ReliableRouter &oracle,unsigned already) {
  f.s.poll(1300,900);
  assert(f.radio.frames.size()==already+oracle.emitted.size());
  if(oracle.emitted.empty())return;
  assert(oracle.emitted.size()==1);
  const auto &e=oracle.emitted[0];const auto &wire=f.radio.frames.back();bench::RouteMetadata m;
  assert(f.s.codec.decode(wire.data(),wire.size(),m));
  assert(bench::le32(wire.data())==e.to&&bench::le32(wire.data()+4)==OURS);
  assert(m.portnum==5&&m.request_id==e.request&&m.routingErrorPresent&&m.routingError==uint32_t(e.code));
  assert((wire[12]&7)==unsigned(e.hop)&&bool(wire[12]&8)==e.reliable);
  assert((wire[12]>>5)==(wire[12]&7)&&wire[14]==0&&m.hasBitfield);
}
int main() {
  unsigned vectors=0;
  // New endpoint next-hop completion versus unchanged stock sniffReceived.
  // Include all accepted header hop pairs, text/reply/reaction and NodeInfo.
  for(unsigned port:{1u,4u})for(unsigned start=0;start<=7;++start)for(unsigned hop=0;hop<=start;++hop)
    for(bool ack:{false,true})for(uint8_t next:{uint8_t(0),uint8_t(OURS),uint8_t(0x99)})
      for(unsigned flavor=0;flavor<(port==1?3u:1u);++flavor) {
        Fixture f;auto p=packet(100,port,start,hop,ack,next);
        if(flavor)p.decoded.reply_id=44;
        if(flavor==2)p.decoded.emoji=0x1f44d;
        stock::ReliableRouter oracle;oracle.sniffReceived(&p,nullptr);
        f.rx(encode(f,p),1000,700);expect(f,oracle,0);++vectors;
      }
  // Matched successful direct-message ACK. Relayed flooding responses must
  // complete the user message without generating a useless zero-hop response.
  for(unsigned start=0;start<=7;++start)for(unsigned hop=0;hop<=start;++hop)
    for(uint8_t next:{uint8_t(0),uint8_t(OURS),uint8_t(0x99)}) {
      Fixture f;f.text();f.s.poll(300,0);f.done();
      auto p=packet(100,5,start,hop,true,next);p.decoded.request_id=1;
      meshtastic_Routing r{};r.which_variant=meshtastic_Routing_error_reason_tag;r.error_reason=meshtastic_Routing_Error_NONE;
      auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
      assert(pb_encode(&s,meshtastic_Routing_fields,&r));p.decoded.payload.size=s.bytes_written;
      stock::ReliableRouter oracle;oracle.sniffReceived(&p,&r);
      f.rx(encode(f,p),1000,700);f.clients();assert(results(f.h,1,0,PEER)==1);
      expect(f,oracle,1);++vectors;
    }
  for(unsigned start=0;start<=7;++start)for(unsigned hop=0;hop<=start;++hop)
    for(bool ack:{false,true})for(uint8_t next:{uint8_t(0),uint8_t(OURS),uint8_t(0x99)}) {
      Fixture f(true,81,true);assert(!f.s.submitNodeInfo(0,77,PEER,3,true,true,100));
      f.s.poll(300,0);f.done();assert(f.radio.sends==1);
      auto p=packet(100,4,start,hop,ack,next);p.decoded.request_id=77;
      stock::ReliableRouter oracle;oracle.sniffReceived(&p,nullptr);
      f.rx(encode(f,p),1000,700);f.clients();assert(results(f.h,77,0,PEER)==1);
      expect(f,oracle,1);++vectors;
    }
  group("1188 stock ACK decision vectors: text, reply, reaction, NodeInfo and matched responses");
  assert(vectors==1188);
  // Legacy packets omit Data.bitfield. A zero hop_start is then unknown,
  // which affects both origin replies and terminal ACK-of-ACK suppression.
  for (bool response : {false, true}) {
    for (unsigned next : {0u, unsigned(uint8_t(OURS))}) {
      if (!response && next) continue;
      Fixture f;
      if (response) { f.text(); f.s.poll(300, 0); f.done(); }
      auto p = packet(100, response ? 5 : 1, 0, 0, true, uint8_t(next));
      p.decoded.has_bitfield = false;
      p.decoded.request_id = response ? 1 : 0;
      meshtastic_Routing success{};
      success.which_variant = meshtastic_Routing_error_reason_tag;
      success.error_reason = meshtastic_Routing_Error_NONE;
      stock::ReliableRouter oracle;
      oracle.sniffReceived(&p, response ? &success : nullptr);
      auto bytes = f.wire(PEER, OURS, 100, p.decoded.request_id, 0, 8, !response, false);
      bytes[14] = p.next_hop;
      f.rx(bytes, 1000, 700);
      if (response) { f.clients(); assert(results(f.h, 1, 0, PEER) == 1); }
      expect(f, oracle, response ? 1 : 0);
    }
  }
  group("three legacy zero-hop ACK decisions match the pinned stock router on encoded packets");
  // New response shares the existing duplicate rate ledger: no more than
  // three jobs for an incoming (sender,id), each terminal and without retries.
  {
    Fixture f;auto p=packet(100,1,3,1,false,uint8_t(OURS));auto wire=encode(f,p);
    for(auto t:{1000u,1200u,3000u,5000u,7000u}) {
      f.rx(wire,t,700+t);f.s.poll(t+300,0);if(f.s.active())f.done(t+301);
    }
    assert(f.radio.sends==3);f.s.poll(100000,0);assert(f.radio.sends==3);
    group("duplicate next-hop completion is bounded and never reliable/retried");
  }
  for(unsigned mode=0;mode<7;++mode) {
    Fixture f(mode!=0);auto p=packet(100,1,3,1,false,uint8_t(OURS));
    if(mode==1)assert(f.s.armEndpoint(false));
    if(mode==2)p.to=OURS+1;
    if(mode==3)p.to=0xffffffffu;
    if(mode==4)p.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;
    auto wire=encode(f,p);
    if(mode==5)wire[13]^=1;
    if(mode==6)wire[12]=uint8_t((1<<5)|2); // impossible hop count
    f.rx(wire,1000,700);f.s.poll(1300,0);assert(!f.radio.sends);
  }
  group("release/disarm, wrong destination, broadcast, unsupported port, wrong key and invalid hops suppress next-hop ACK");
  {
    Fixture f;auto p=packet(100,4,3,1,false,uint8_t(OURS));p.decoded.bitfield=2;
    f.rx(encode(f,p),1000,700);f.s.poll(1300,0);assert(!f.radio.sends);
    // A correlated-looking but unmatched NodeInfo response is not accepted.
    p.decoded.bitfield=0;p.decoded.request_id=55;p.id=101;
    f.rx(encode(f,p),2000,701);f.s.poll(2300,0);assert(!f.radio.sends);
    group("NodeInfo queries and unmatched responses do not allocate a parallel completion ACK");
  }
  {
    Fixture f(true,81,true);auto p=packet(100,4,3,1,false,uint8_t(OURS));p.decoded.bitfield=2;
    f.rx(encode(f,p),1000,700);f.s.poll(1300,0);assert(f.radio.sends==1);
    bench::RouteMetadata m;const auto &wire=f.radio.frames[0];
    assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.portnum==4&&m.request_id==100);
    f.done(1400);f.s.poll(1800,0);assert(f.radio.sends==1);
    group("accepted discovery query has only its NodeInfo response, no parallel terminal ACK");
  }
  {
    Fixture f;f.text();f.s.poll(300,0);f.done();
    auto wire=f.wire(PEER,OURS,100,1,0,uint8_t((3<<5)|1));wire[14]=uint8_t(OURS);
    f.rx(wire,1000,700);f.clients();assert(results(f.h,1,0,PEER)==1);
    f.s.poll(1300,0);assert(f.radio.sends==1);
    group("routing response without want_ack completes user send without starting an ACK loop");
  }
  {
    Fixture f;f.radio.sendStatus=-EBUSY;auto p=packet(100,1,3,1,false,uint8_t(OURS));
    f.rx(encode(f,p),1000,700);f.s.poll(1300,0);
    assert(f.radio.sends==1&&!f.s.active()&&!f.s.faulted()&&!f.s.sent);
    f.s.poll(1549,0);assert(f.radio.sends==1);
    f.radio.sendStatus=0;f.s.poll(1550,0);
    assert(f.radio.sends==2&&f.s.active()&&f.radio.frames[0]==f.radio.frames[1]);
    f.done(1700);assert(f.s.sent==1&&!f.s.active()&&!f.s.faulted()&&!f.s.endpointControls());
    f.s.poll(2000,0);f.s.poll(100000,0);assert(f.radio.sends==2);
    group("next-hop completion retries the identical unaccepted EBUSY frame then completes once");
  }
  {
    Fixture f;f.radio.sendStatus=-EBUSY;auto p=packet(100,1,3,1,false,uint8_t(OURS));
    f.rx(encode(f,p),1000,700);f.s.poll(1300,0);f.s.poll(1550,0);f.s.poll(1800,0);
    assert(f.radio.sends==3&&!f.s.active()&&!f.s.faulted()&&!f.s.sent&&!f.s.endpointControls());
    assert(f.radio.frames[0]==f.radio.frames[1]&&f.radio.frames[1]==f.radio.frames[2]);
    f.s.poll(2050,0);f.s.poll(5000,0);f.s.poll(100000,0);assert(f.radio.sends==3);
    group("native EBUSY controls have at most three attempts and no send after exhaustion");
  }
  {
    Fixture f;
    for(unsigned i=0;i<5;++i)f.rx(encode(f,packet(100+i,1,3,1,false,uint8_t(OURS))),1000,700+i);
    for(unsigned i=0;i<5;++i){const unsigned t=1300+i*500;f.s.poll(t,0);if(f.s.active())f.done(t+100);}
    assert(f.radio.sends==4);
    f.rx(encode(f,packet(200,1,3,1,false,uint8_t(OURS))),5000,800);
    assert(f.s.armEndpoint(false));f.s.poll(5300,0);assert(f.radio.sends==4);
    group("next-hop completion respects four control slots and disarm cancels queued response");
  }
  {
    Fixture f;f.radio.air=12000;
    for(unsigned i=0;i<3;++i) {
      auto p=packet(100+i,1,3,1,false,uint8_t(OURS));const unsigned t=1000+i*4000;
      f.rx(encode(f,p),t,700+i);f.s.poll(t+300,0);if(f.s.active())f.done(t+2500);
    }
    assert(f.radio.sends==2&&f.s.throttled==1&&!f.s.faulted());
    group("next-hop ACKs stop at the normal-mode 40 percent channel-utilization threshold");
  }
  std::cout<<"PASS endpoint routing increment; host only, no hardware or RF\n";
}
