// Independent M94 review adversary adopted by root; original failed probe is preserved.
// Actual Companion admission with a recording private boundary; no private crypto/RF.
#include "endpoint_fixture.h"
#include "known_channel_fixture.h"
#include <cstring>
struct CaptureRadio:Radio {
  CaptureRadio(){healthStatus=0;}
  int start()override{healthStatus=1;return 0;}
  int stop()override{healthStatus=0;return Radio::stop();}
  int restore()override{healthStatus=1;return 0;}
};
struct Capture:mt::pki::PrivateRuntime {
 unsigned calls=0,lastHop=99,lastPort=0;bool lastAck=false;
 uint32_t peer()const override{return PEER;}
 bool ready(uint32_t node)const override{return node==OURS;}
 bool publicIdentity(uint8_t out[32])const override{memset(out,0x42,32);return true;}
 mt::pki::DestinationDisposition destinationDisposition(uint32_t node,const uint8_t *key,size_t size)const override{
  if(node!=PEER)return mt::pki::DestinationDisposition::Unavailable;
  if(size)return mt::pki::DestinationDisposition::KeyMismatch;
  if(key)return mt::pki::DestinationDisposition::Unavailable;
  return mt::pki::DestinationDisposition::PinnedActive;
 }
 bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
 void freeze()override{} bool matchesPeer(uint32_t node)const override{return node==PEER;}
 bool revoke(uint32_t)override{return false;}void clearEvent()override{}void sweep(mt::Endpoint &)override{}
 int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *data,size_t n,unsigned hop,bool ack,const uint8_t *,size_t,bool,uint32_t)override{
  bench::RouteMetadata m;assert(bench::MtCodec::decodeData(data,n,m));++calls;lastHop=hop;lastPort=m.portnum;lastAck=ack;return 0;
 }
 void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
 void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
 bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
struct Probe {
  mt::test::KnownChannelFixtureClock knownChannelClock;
 Capture capture;CaptureRadio radio;mt::Service service;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,b;
 Probe(unsigned stored,bool privateMode):service(radio,true,OURS,81,true,false,privateMode?&capture:nullptr),usb(service),ble(service),c(service,usb,81,workspace,0),b(service,ble,81,workspace,1){
    knownChannelClock.bind(service);
  auto config=service.config;config.bytes[4]=1;assert(config.setHopLimit(stored));config.seal();
  assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
  meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=1;input(c,hello,1);input(b,hello,1);usb.out.clear();ble.out.clear();
 }
};
static meshtastic_ToRadio packet(unsigned port,unsigned hop,bool ack){
 meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
 p.id=701;p.to=PEER;p.hop_limit=hop;p.want_ack=ack;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=static_cast<meshtastic_PortNum>(port);
 if(port==3||port==67||port==71)p.decoded.want_response=true;
 if(port==67){p.decoded.payload.size=2;p.decoded.payload.bytes[0]=0x32;p.decoded.payload.bytes[1]=0;}
 else if(port!=3&&port!=71){p.decoded.payload.size=1;p.decoded.payload.bytes[0]='x';}
 return r;
}
int main(){
 (void)&results;(void)&group;
 const bool expectFixed=true;unsigned failures=0,rows=0;
 for(unsigned stored:{0u,1u,7u})for(bool owner:{false,true})for(unsigned port:{1u,67u,71u,256u})for(bool ack:{false,true})for(unsigned requested:{0u,2u,7u}){
  auto f=std::make_unique<Probe>(stored,true);auto r=packet(port,requested,ack);input(owner?f->b:f->c,r,100);
  assert(f->capture.calls==1&&f->capture.lastPort==port&&f->capture.lastAck==ack&&!f->radio.sends&&!f->service.faulted());
  const auto expected=(ack&&!requested)?stored:requested;const bool same=f->capture.lastHop==expected;failures+=!same;++rows;
  std::cout<<"{\"private\":true,\"stored\":"<<stored<<",\"BLE\":"<<owner<<",\"port\":"<<port<<",\"want_ack\":"<<ack<<",\"requested\":"<<requested<<",\"expected\":"<<expected<<",\"observed\":"<<f->capture.lastHop<<",\"match\":"<<same<<"}\n";
 }
 // POSITION is excluded from stock automatic PKI selection. Its ordinary
 // want_response query must retain the channel route and requested hop.
 for(bool owner:{false,true})for(unsigned requested:{0u,2u,7u}){
  auto f=std::make_unique<Probe>(7,true);auto r=packet(3,requested,false);input(owner?f->b:f->c,r,100);f->service.poll(300,0);
  assert(!f->capture.calls&&f->radio.frames.size()==1&&!f->service.faulted());
  const auto observed=unsigned(f->radio.frames[0][12]&7u);const bool same=observed==requested;failures+=!same;++rows;
  std::cout<<"{\"private\":false,\"excluded\":true,\"stored\":7,\"BLE\":"<<owner<<",\"port\":3,\"want_ack\":false,\"requested\":"<<requested<<",\"expected\":"<<requested<<",\"observed\":"<<observed<<",\"match\":"<<same<<"}\n";
 }
 // An explicit PKI claim on the same excluded port is a refusal, never a
 // covert channel downgrade.
 for(bool owner:{false,true}){
  auto f=std::make_unique<Probe>(7,true);auto r=packet(3,2,false);r.packet.pki_encrypted=true;input(owner?f->b:f->c,r,100);
  const bool refused=!f->capture.calls&&!f->radio.sends&&
    results(owner?f->ble:f->usb,r.packet.id,meshtastic_Routing_Error_PKI_FAILED)==1;
  assert(refused&&!f->service.faulted());failures+=!refused;++rows;
  std::cout<<"{\"private\":false,\"excluded\":true,\"explicit_pki\":true,\"BLE\":"<<owner<<",\"port\":3,\"expected_error\":34,\"match\":"<<refused<<"}\n";
 }
 for(bool owner:{false,true}){
  auto f=std::make_unique<Probe>(7,false);auto r=packet(256,0,true);input(owner?f->b:f->c,r,100);f->service.poll(300,0);
  assert(f->radio.frames.size()==1&&!f->service.faulted());const auto observed=f->radio.frames[0][12]&7;failures+=observed!=7;++rows;
  std::cout<<"{\"private\":false,\"stored\":7,\"BLE\":"<<owner<<",\"port\":256,\"want_ack\":true,\"requested\":0,\"expected\":7,\"observed\":"<<observed<<",\"match\":"<<(observed==7)<<"}\n";
 }
 std::cout<<"{\"summary\":true,\"vectors\":"<<rows<<",\"mismatches\":"<<failures<<",\"expect_fixed\":"<<expectFixed<<"}\n";
 return int(failures!=0);
}
