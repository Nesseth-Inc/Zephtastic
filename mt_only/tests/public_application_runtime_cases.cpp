// SPDX-License-Identifier: GPL-3.0-only
// Real PSA identity/ledger, mock radio, synthetic public keys. No devices.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"
namespace {
void group(const char *name){std::cout<<"PASS "<<name<<'\n';}
Bytes application(unsigned port=8,uint8_t bitfield=0){uint8_t out[239]{};size_t n=0;const uint8_t payload[]={0,0xff,0x80,1};
  assert(mt::PublicApplication::encode(port,payload,sizeof(payload),23,0x1f44d,out,sizeof(out),n,bitfield));return {out,out+n};}
void custom(Fixture &f,size_t width){
  if(!width)return;
  f.s.stop();auto cfg=f.s.config;auto record=cfg.node();uint8_t key[32]{};key[0]=0x51;
  mt::PrimaryChannel::Selection selected;assert(mt::PrimaryChannel::normalize(key,width,selected));record.setKey(selected.hash,selected.bytes,selected.length);
  mt::PrimaryChannel::clear(selected);cfg.node(record);assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));}
meshtastic_ToRadio request(Fixture &f,uint32_t id=201,uint32_t to=0){auto r=f.request(id);auto &p=r.packet;p.from=0;p.to=to?to:f.peer.binding.node;p.hop_limit=0;
  p.decoded.portnum=meshtastic_PortNum_WAYPOINT_APP;p.decoded.payload.size=4;memcpy(p.decoded.payload.bytes,"app!",4);return r;}
struct OldProvider:mt::pki::PrivateRuntime {
  bool available=true;
  uint32_t peer()const override{return 42;}bool ready(uint32_t)const override{return available;}
  bool publicIdentity(uint8_t [32])const override{return false;}
  bool bootstrap(bench::MtCodec &,const bench::MtNodeRecord &,uint8_t,uint32_t,uint32_t,bool,uint32_t,bench::Frame &)override{return false;}
  void freeze()override{available=false;}bool matchesPeer(uint32_t)const override{return false;}bool revoke(uint32_t)override{return false;}
  void clearEvent()override{}void sweep(mt::Endpoint &)override{}
  int submit(mt::Endpoint &,uint8_t,uint32_t,uint32_t,const uint8_t *,size_t,unsigned,bool,const uint8_t *,size_t,bool,uint32_t)override{return -ENOTSUP;}
  void receive(mt::Endpoint &,bench::MtCodec &,const uint8_t *,size_t,uint32_t,uint32_t,bool)override{}
  void proof(mt::Endpoint &,const uint8_t *,size_t,const bench::RouteMetadata &,uint32_t)override{}
  bool event(uint8_t,const uint8_t *,size_t,mt::pki::PrivateEvent &)override{return false;}
};
}
int main(){
  for(size_t width:{size_t(0),size_t(16),size_t(32)})for(uint8_t consent:{0,1}){unsigned accepted=0;
    // The legacy memory-journal fixture has 144 send slots. Keep each fresh
    // synthetic fixture under that bound; production append capacity is separate.
    for(unsigned bucket=0;bucket<4;++bucket){Fixture f;custom(f,width);
    for(unsigned port=bucket*128;port<(bucket+1)*128;++port)if(mt::PublicApplication::allowed(port)){
      const auto plain=application(port,consent);const uint32_t id=2000+port;const auto entropy=f.entropy.calls;bench::Frame frame;
      assert(f.s.encodePublicApplication(f.peer.binding.node,id,plain.data(),plain.size(),frame)&&f.ledger.sent(id)&&f.entropy.calls==entropy+1);
      bench::RouteMetadata m;assert(f.s.codec.decode(frame.bytes,frame.len,m)&&m.portnum==port&&m.hasBitfield&&m.bitfield==consent&&!m.wantResponse&&!m.signatureLength&&
        m.payloadLength==4&&m.reply_id==23&&m.emoji==0x1f44d&&bench::le32(frame.bytes+4)==f.local.binding.node&&frame.bytes[13]);
      assert(!f.radio.sends&&f.s.receiving());++accepted;
    }}
    assert(accepted>256);
  }
  group("all allowed ports and both MQTT consent choices on default AES128 and custom AES128 AES256 reserve one durable ID and encode unsigned channel data");
  for(size_t width:{size_t(0),size_t(16),size_t(32)})for(uint8_t consent:{0,1})for(uint32_t to:{UINT32_MAX}){Fixture f(true);custom(f,width);auto r=request(f,201,to);
    r.packet.decoded.has_bitfield=true;r.packet.decoded.bitfield=consent;
    const auto stops=f.radio.stops,restores=f.radio.restores;input(f.c,r,100);input(f.bc,r,101);
    assert(f.entropy.calls==1&&f.signing.calls==1&&f.ledger.sent(201)&&!f.backend.encrypts&&f.s.endpointFree()==3&&f.radio.stops==stops+1&&f.radio.restores==restores+1);
    f.poll(1000);assert(f.radio.frames.size()==1);f.complete(1200);
    bench::RouteMetadata m;const auto &wire=f.radio.frames[0];assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.portnum==8&&m.hasBitfield&&m.bitfield==consent&&m.payloadLength==4&&m.signatureLength==64&&
      m.reply_id==0&&!m.emoji&&!m.wantResponse&&
      bench::le32(wire.data()+4)==f.local.binding.node&&bool(wire[12]&8)==(r.packet.to!=UINT32_MAX));
    bench::MtSignature verifier;assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,1000));
    assert(verifier.apply(wire.data(),wire.size(),m,1000)==bench::MtSignature::Result::Verified);
    auto different=r;different.packet.decoded.payload.bytes[0]^=1;input(f.c,different,1300);
    assert(f.entropy.calls==1&&f.signing.calls==1&&!f.backend.encrypts&&f.radio.frames.size()==1&&f.result(201,false,meshtastic_Routing_Error_BAD_REQUEST)==1);
  }
  group("generic broadcasts use verified signatures and same-ID changed plaintext is refused; private unicast policy covered separately");
  for(size_t width:{size_t(0),size_t(16),size_t(32)}){Fixture f;custom(f,width);const auto plain=application();bench::Frame frame;const auto entropy=f.entropy.calls;
    assert(f.s.encodePublicApplication(f.peer.binding.node,201,plain.data(),plain.size(),frame));
    auto changed=plain;changed[4]^=1;const auto snapshot=frame;
    assert(!f.s.encodePublicApplication(f.peer.binding.node,201,changed.data(),changed.size(),frame)&&f.entropy.calls==entropy+1&&frame.len==snapshot.len&&!memcmp(frame.bytes,snapshot.bytes,sizeof(frame.bytes)));
    const Bytes invalid={8,6,72,0};assert(!f.s.encodePublicApplication(f.peer.binding.node,202,invalid.data(),invalid.size(),frame)&&!f.ledger.sent(202));
    auto wanted=plain;wanted.insert(wanted.end(),{24,1});assert(!f.s.encodePublicApplication(f.peer.binding.node,203,wanted.data(),wanted.size(),frame)&&!f.ledger.sent(203));
  }
  group("durable generic IDs cannot be re-encrypted with changed plaintext and invalid management response forms consume no nonce");
  for(unsigned mode=0;mode<5;++mode){Fixture f;const auto plain=application();bench::Frame out;memset(out.bytes,0x42,sizeof(out.bytes));out.len=99;const auto before=out;
    if(mode==0)f.radio.busyValue=true;
    if(mode==1)f.entropy.fail=true;
    if(mode==2)f.radio.restoreError=-EIO;
    if(mode==3)f.lm.fault=2;
    if(mode==4)f.runtime.freeze();
    assert(!f.s.encodePublicApplication(f.peer.binding.node,201,plain.data(),plain.size(),out)&&out.len==before.len&&!memcmp(out.bytes,before.bytes,sizeof(out.bytes))&&!f.radio.sends);
    if(mode==0||mode==4)assert(!f.entropy.calls&&!f.ledger.sent(201));
    if(mode==1)assert(f.ledger.sent(201)&&f.s.receiving()&&!f.s.faulted());
    if(mode==2||mode==3)assert(f.s.faulted()&&!f.s.receiving());
  }
  group("busy unready reservation entropy uncertain-write and restore failures preserve output with no unsigned fallback");
  {Radio radio;OldProvider old;mt::Service service(radio,true,0x12345678,81,true,false,&old);mt::Config cfg;cfg.bytes[4]=1;auto record=cfg.node();record.setPublic();cfg.node(record);
    assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));const auto plain=application();bench::Frame frame;frame.len=99;
    assert(!service.encodePublicApplication(0x87654321,201,plain.data(),plain.size(),frame)&&frame.len==99&&service.receiving()&&!radio.sends);
    assert(service.submitPublicApplication(0,0x87654321,201,plain.data(),plain.size(),0,false,100)<0&&service.endpointFree()==4&&!radio.sends);
  }
  group("older configured provider default-refuses generic application hook without channel fallback or queue publication");
  {Fixture f;PkiPeerPin pin{};pin.node=f.peer.binding.node;memcpy(pin.publicKey,f.peer.binding.publicKey,32);PkiPeerSet set;
    assert(set.assign(f.local.binding,&pin,1));MultiPeerRuntime multi(f.crypto,f.backend,f.identity,f.nonce,f.replay);
    assert(multi.open(f.local.binding.node,set));const auto plain=application(511);bench::Frame frame;Key stranger(193);
    assert(f.s.beginPrivateAdministration());
    assert(multi.encodePublicApplication(f.s.codec,stranger.binding.node,7001,plain.data(),plain.size(),frame));
    assert(f.s.endPrivateAdministration()&&f.ledger.sent(7001)&&f.entropy.calls==1&&!f.backend.encrypts);
    bench::RouteMetadata m;assert(f.s.codec.decode(frame.bytes,frame.len,m)&&m.portnum==511&&!m.signatureLength&&
      bench::le32(frame.bytes)==stranger.binding.node&&bench::le32(frame.bytes+4)==f.local.binding.node);
    assert(!f.s.encodePublicApplication(stranger.binding.node,7001,plain.data(),plain.size(),frame)&&f.entropy.calls==1&&!f.radio.sends);
  }
  group("multi-peer facade delegates unsigned generic transport to unpinned channel destinations in one shared durable ID domain");
  for(size_t width:{size_t(0),size_t(16),size_t(32)})for(uint8_t consent:{0,1}){Fixture f;custom(f,width);const auto plain=application(8,consent);uint8_t raw[255]{};size_t n=0;
    assert(f.s.codec.encodeZeroHopData(f.peer.binding.node,f.local.binding.node,901,plain.data(),plain.size(),raw,sizeof(raw),n));raw[12]=8;
    Bytes wire(raw,raw+n);f.rx(wire,1000);assert(f.entropy.calls==1&&f.ledger.sent(81)&&!f.radio.sends);
    for(const auto *host:{&f.usb,&f.ble}){unsigned count=0;
      for(const auto &event:host->out)if(event.which_payload_variant==meshtastic_FromRadio_packet_tag&&event.packet.decoded.portnum==8){
        assert(event.packet.decoded.has_bitfield&&event.packet.decoded.bitfield==consent);++count;
      }
      assert(count==1);
    }
    f.poll(2000);assert(f.radio.frames.size()==1);bench::RouteMetadata m;const auto &ack=f.radio.frames[0];assert(f.s.codec.decode(ack.data(),ack.size(),m)&&m.portnum==5&&m.request_id==901&&m.routingErrorPresent&&!m.routingError);
    f.complete(2200);f.rx(wire,2500);f.poll(2600);assert(f.entropy.calls==1&&f.radio.frames.size()==1);
  }
  group("generic automatic ACK reserves durable ID even on default channel and duplicates reuse bounded control ciphertext");
  {Fixture f;const auto plain=application();uint8_t raw[255]{};size_t n=0;assert(f.s.codec.encodeZeroHopData(f.peer.binding.node,f.local.binding.node,901,plain.data(),plain.size(),raw,sizeof(raw),n));raw[12]=8;
    f.entropy.fail=true;f.rx(Bytes(raw,raw+n),1000);f.poll(2000);assert(f.entropy.calls==1&&f.ledger.sent(81)&&f.radio.frames.empty()&&f.s.receiving());
  }
  group("generic automatic ACK reservation failure never queues unreserved fallback");
}
