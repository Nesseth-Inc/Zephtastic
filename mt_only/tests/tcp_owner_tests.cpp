// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "network/MtTcpTxQueue.h"
struct QueueHost final:Host {
 mt::tcp::FixedTxQueue<64> queued;unsigned dropped=0;
 explicit QueueHost(mt::Service&s):Host(s){}
 bool send(const uint8_t*p,size_t n)override{
  if(!queued.pushFramed(p,n)){++dropped;return false;}
  return Host::send(p,n);
 }
};
static void text(mt::Companion&c,uint32_t id){meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto&p=r.packet;p.id=id;p.to=PEER;p.want_ack=true;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=5;memcpy(p.decoded.payload.bytes,"Hello",5);input(c,r,100);}
int main(){
 static_assert(mt::LocalClientCount==3);
 {
  mt::tcp::FixedTxQueue<4> q;
  const uint8_t frame[]={0x94,0xc3,0,1,8};
  assert(q.pushFramed(frame,sizeof(frame),true));
  assert(!q.pushFramed(frame,sizeof(frame),true));
  assert(q.pushFramed(frame,sizeof(frame)));
  mt::tcp::FixedTxQueue<4>::Frame first,second;
  assert(q.pop(first)&&q.pop(second)&&q.pushFramed(frame,sizeof(frame),true));
 }
 {
  Fixture f;
  for(unsigned i=0;i<mt::NodeCache::Capacity;++i){mt::NodeCache::Entry node;node.identity(PEER+i,81,1);assert(f.s.nodes.accept(node,OURS,1,0)>=0);}
  assert(f.s.nodes.count()==16);QueueHost host(f.s);mt::companion::CompanionWorkspace workspace;mt::Companion tcp(f.s,host,81,workspace,2);
  meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=27;input(tcp,hello,1);
  assert(host.dropped==0&&tcp.outputDrops==0&&host.out.size()>48&&host.out.size()<=64);
  mt::tcp::FixedTxQueue<64>::Frame frame;unsigned drained=0;while(host.queued.pop(frame))++drained;assert(drained==host.out.size());
  group("capacity64 queue accepts entire synchronous maximum-node handshake without loss");
 }
 {
  Fixture f;Host third(f.s);mt::companion::CompanionWorkspace workspace;mt::Companion tcp(f.s,third,81,workspace,2);meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=9;input(tcp,hello,1);assert(third.out.size()>30);third.out.clear();
  text(tcp,7);f.text(7,PEER,true,100,0);f.text(7,PEER,true,100,1);assert(f.s.endpointFree()==3);f.s.poll(300,0);f.done();f.rx(f.wire(PEER,OURS,100,7));f.clients();tcp.poll(mt::Companion::WorkspaceProof{true});
  assert(f.radio.sends==1&&results(f.h,7,0,PEER)==1&&results(f.bh,7,0,PEER)==1&&results(third,7,0,PEER)==1);
  group("USB BLE TCP share one RF job with independent result ownership");
 }
 {
  Fixture f;Host third(f.s);mt::companion::CompanionWorkspace workspace;mt::Companion tcp(f.s,third,81,workspace,2);text(tcp,9);f.text(9,PEER,true,100,0);tcp.resetStream();assert(f.s.endpointFree()==3);f.s.poll(300,0);f.done();f.rx(f.wire(PEER,OURS,101,9));f.clients();tcp.poll(mt::Companion::WorkspaceProof{true});assert(results(f.h,9,0,PEER)==1&&results(third,9,0)==0);
  mt::Endpoint::Result result;assert(!f.s.takeMessageResult(3,result));auto wire=f.wire(OURS,PEER,14,0,0,0,true);assert(f.s.submitMessage(3,wire.data(),wire.size(),false,1000)==-EINVAL);
  group("TCP disconnect removes only TCP owner and out-of-range owner is refused");
 }
 {
  Fixture f;mt::Endpoint ep;bench::Frame frame;size_t size=0;assert(f.s.encodeTelemetryQuery(PEER,201,frame.bytes,sizeof(frame.bytes),size));frame.len=size;frame.bytes[12]=8;
  for(uint8_t owner:{0,1,2}){assert(!ep.submit(owner,frame.bytes,frame.len,true,100,false,true,true,nullptr,true,false,false,true));}
  auto*j=ep.next(100);assert(j);ep.started(*j,0,200,100,0);ep.complete(*j,300,0);ep.authenticated(201,PEER,0);mt::Endpoint::Result result;
  for(uint8_t owner:{0,1,2}){assert(ep.take(owner,result)&&result.kind==mt::Endpoint::Kind::Authenticated&&result.id==201);}
  assert(ep.pendingPrivate(201,PEER));ep.disconnect(2);assert(ep.pendingPrivate(201,PEER));
  group("modeled private query authentication phase and disconnect preserve all three owner masks");
 }
}
