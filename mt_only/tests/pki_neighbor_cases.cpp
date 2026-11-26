// SPDX-License-Identifier: GPL-3.0-only
// Synthetic Service/Companion transactions with the real PSA backend. No devices.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"
#include "pki/PrivateApplication.h"

namespace {
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
Bytes encoded(const meshtastic_Data &data){
  uint8_t raw[227]{};auto out=pb_ostream_from_buffer(raw,sizeof(raw));
  assert(pb_encode(&out,meshtastic_Data_fields,&data));return {raw,raw+out.bytes_written};
}
meshtastic_Data decoded(const Bytes &plain){
  meshtastic_Data data{};auto in=pb_istream_from_buffer(plain.data(),plain.size());
  assert(pb_decode(&in,meshtastic_Data_fields,&data)&&!in.bytes_left&&encoded(data)==plain);return data;
}
Bytes query(){uint8_t raw[16]{};size_t size=0;assert(mt::NeighborQuery::request(raw,sizeof(raw),size));return {raw,raw+size};}
meshtastic_NeighborInfo snapshot(uint32_t node,bool populated=false){
  meshtastic_NeighborInfo info{};info.node_id=node;info.last_sent_by_id=node;info.node_broadcast_interval_secs=3600;
  if(populated){info.neighbors_count=1;info.neighbors[0].node_id=0x12345678;info.neighbors[0].snr=-3.25f;
    info.neighbors[0].last_rx_time=123;info.neighbors[0].node_broadcast_interval_secs=3600;}
  return info; // Explicit synthetic RAM observations, never inferred from a request.
}
Bytes response(uint32_t node,uint32_t request,bool populated=false){
  uint8_t raw[227]{};size_t size=0;
  assert(mt::NeighborQuery::response(request,snapshot(node,populated),raw,sizeof(raw),size));return {raw,raw+size};
}
Bytes cipher(Fixture &f,const Bytes &plain,uint32_t id,const Key *sender=nullptr){
  const auto &key=sender?*sender:f.peer;uint8_t raw[239]{};size_t size=0;
  assert(f.crypto.encrypt(key.scalar,f.local.binding.publicKey,key.binding.node,id,0x19374628,
    plain.data(),plain.size(),raw,sizeof(raw),size));
  Bytes frame(16+size);put(frame.data(),f.local.binding.node);put(frame.data()+4,key.binding.node);put(frame.data()+8,id);
  frame[15]=uint8_t(key.binding.node);memcpy(frame.data()+16,raw,size);return frame;
}
Bytes peerDecrypt(Fixture &f,const Bytes &frame){
  uint8_t raw[227]{};size_t size=0;
  assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,bench::le32(frame.data()+8),
    frame.data()+16,frame.size()-16,raw,sizeof(raw),size));return {raw,raw+size};
}
meshtastic_ToRadio request(Fixture &f,uint32_t id=201,bool ack=true){
  auto r=f.request(id);auto &p=r.packet;p.from=0;p.hop_limit=0;p.want_ack=ack;p.decoded={};
  p.decoded.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;p.decoded.want_response=true;return r;
}
void originate(Fixture &f,unsigned owners=3,bool ack=true){
  auto r=request(f,201,ack);if(owners&1)input(f.c,r,100);if(owners&2)input(f.bc,r,101);
  f.poll(1000);assert(f.radio.frames.size()==1);f.complete(1200);
}
unsigned replies(const Host &host,uint32_t request=201){
  unsigned count=0;for(const auto &event:host.out)if(event.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    event.packet.decoded.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&event.packet.decoded.request_id==request)++count;
  return count;
}
void delivered(const Fixture &f,const Host &host,const Bytes &expected,uint32_t id,unsigned count=1){
  assert(replies(host)==count);
  for(const auto &event:host.out)if(event.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    event.packet.decoded.portnum==meshtastic_PortNum_NEIGHBORINFO_APP&&event.packet.decoded.request_id==201){
    const auto &p=event.packet;assert(p.id==id&&p.from==f.peer.binding.node&&p.to==f.local.binding.node&&
      p.pki_encrypted&&!p.xeddsa_signed&&p.public_key.size==32&&!memcmp(p.public_key.bytes,f.peer.binding.publicKey,32)&&
      encoded(p.decoded)==expected);
  }
}
struct Snapshot {
  mt::Service *service;meshtastic_NeighborInfo value;unsigned calls=0;bool available=true,privateExpected=true;
  static bool read(void *ctx,meshtastic_NeighborInfo &out){
    auto &self=*static_cast<Snapshot *>(ctx);++self.calls;
    if(self.privateExpected)assert(self.service->privateStoragePaused()&&!self.service->receiving());
    else assert(self.service->receiving()&&!self.service->privateStoragePaused());
    out=self.value;return self.available;
  }
};
void bind(Fixture &f,Snapshot &provider){
  f.s.stop();assert(!f.s.receiving()&&f.s.storageStopSafe());
  assert(f.s.bindNeighborSnapshot(Snapshot::read,&provider));
  assert(!f.s.bindNeighborSnapshot(Snapshot::read,&provider));
  assert(f.s.start(false,0)&&f.s.armEndpoint(true));
}
Bytes publicQuery(mt::Service &service,uint32_t from,uint32_t request){
  uint8_t raw[255]{};size_t size=0;
  assert(service.codec.encodeNeighborQuery(from,service.route.identity(),request,raw,sizeof(raw),size));return {raw,raw+size};
}
}

int main(){
  std::cout.setf(std::ios::unitbuf);
  const auto selector=query();assert(selector==Bytes({8,71,24,1,72,2})&&PrivateApplication::allowed(selector.data(),selector.size()));
  for(unsigned owners:{1u,2u,3u})for(bool ack:{false,true}){Fixture f;const auto config=f.s.config;
    originate(f,owners,ack);const auto &wire=f.radio.frames[0];
    assert(!wire[13]&&bool(wire[12]&8)==ack&&bench::le32(wire.data())==f.peer.binding.node&&
      bench::le32(wire.data()+4)==f.local.binding.node&&peerDecrypt(f,wire)==query());
    assert(f.backend.encrypts==1&&f.entropy.calls==1&&f.ledger.sent(201)&&f.s.endpointFree()==3);
    if(ack){f.rx(f.ack(201),1300);f.poll(1301);assert(f.s.endpointFree()==3&&!f.s.storageStopSafe());
      if(owners&1)assert(f.result(201,true,0)==1);
      auto same=request(f);if(owners&1)input(f.c,same,1302);if(owners&2)input(f.bc,same,1303);
      assert(f.backend.encrypts==1&&f.entropy.calls==1&&f.s.endpointFree()==3);}
    const auto plain=response(f.peer.binding.node,201,true),incoming=cipher(f,plain,9001);
    f.rx(incoming,1500);f.poll(1501);delivered(f,f.usb,plain,9001,owners&1?1:0);delivered(f,f.ble,plain,9001,owners&2?1:0);
    const auto writes=f.lm.writes;f.rx(incoming,1600);f.c.receive(incoming.data(),incoming.size(),-80,3,1601,mt::Companion::WorkspaceProof{true});
    f.bc.receive(incoming.data(),incoming.size(),-80,3,1601,mt::Companion::WorkspaceProof{true});f.poll(1700);
    assert(f.lm.writes==writes&&f.s.endpointFree()==4&&f.s.storageStopSafe()&&f.s.config.same(config)&&f.radio.frames.size()==1);
    delivered(f,f.usb,plain,9001,owners&1?1:0);delivered(f,f.ble,plain,9001,owners&2?1:0);
  }
  pass("canonical private Neighbor query decrypts at pinned peer; proof retains application phase and exact reply reaches only requesting USB BLE owners once");

  for(unsigned mode=0;mode<9;++mode){
    Fixture f;auto r=request(f);const auto writes=f.lm.writes;
    switch(mode){case 0:r.packet.from=7;break;case 1:r.packet.decoded.payload.size=1;break;
      case 2:r.packet.decoded.has_bitfield=true;break;case 3:r.packet.decoded.request_id=7;break;
      case 4:r.packet.to=UINT32_MAX;r.packet.pki_encrypted=true;break;
      case 5:r.packet.to=Key(61).binding.node;break;
      case 6:r.packet.pki_encrypted=true;r.packet.public_key.size=32;
        memset(r.packet.public_key.bytes,0x91,32);break;
      case 7:r.packet.xeddsa_signed=true;break;case 8:f.runtime.freeze();break;}
    input(f.c,r,100);f.poll(1000);assert(!f.backend.encrypts&&!f.entropy.calls&&f.lm.writes==writes&&
      !f.ledger.sent(201)&&f.radio.frames.empty()&&f.s.endpointFree()==4);
  }
  pass("noncanonical caller fields unknown pin explicit wrong key broadcast crypto claims and unavailable provider refuse before nonce without Public fallback");

  {
    Fixture baseline,hinted;
    auto ordinary=request(baseline),withHint=request(hinted);
    withHint.packet.public_key.size=32;
    memset(withHint.packet.public_key.bytes,0x91,32);
    input(baseline.c,ordinary,100);input(hinted.c,withHint,100);
    baseline.poll(1000);hinted.poll(1000);
    assert(baseline.radio.frames.size()==1&&hinted.radio.frames==baseline.radio.frames);
    const auto &wire=hinted.radio.frames[0];
    assert(!wire[13]&&bench::le32(wire.data())==hinted.peer.binding.node&&
      bench::le32(wire.data()+4)==hinted.local.binding.node&&peerDecrypt(hinted,wire)==query());
    assert(baseline.backend.encrypts==1&&hinted.backend.encrypts==1&&
      baseline.entropy.calls==1&&hinted.entropy.calls==1&&
      baseline.ledger.sent(201)&&hinted.ledger.sent(201));
    baseline.complete(1200);hinted.complete(1200);
    baseline.rx(baseline.ack(201),1300);hinted.rx(hinted.ack(201),1300);
    baseline.poll(1301);hinted.poll(1301);
    assert(baseline.result(201,true,0)==1&&hinted.result(201,true,0)==1);
  }
  pass("bounded key hint without explicit PKI is ignored; pinned peer key produces identical encrypted Neighbor query and normal ACK completion");

  {Fixture f;originate(f);const auto plain=response(f.peer.binding.node,201);auto tampered=cipher(f,plain,9001);tampered.back()^=1;
    const auto writes=f.lm.writes;f.rx(tampered,1300);Key other(61);f.rx(cipher(f,plain,9002,&other),1310);
    auto malformed=decoded(plain);malformed.payload.bytes[malformed.payload.size++]=0;
    f.rx(cipher(f,encoded(malformed),9003),1320);assert(f.lm.writes==writes&&!replies(f.usb)&&!replies(f.ble)&&f.s.endpointFree()==3);
    uint8_t metrics[80]{};size_t size=0;
    assert(mt::TelemetryQuery::uptimeResponse(123,201,zt::Observed<float>{0.0f},zt::Observed<float>{0.0f},metrics,sizeof(metrics),size));
    f.rx(cipher(f,Bytes(metrics,metrics+size),9004),1330);
    f.rx(cipher(f,response(f.peer.binding.node,202),9005),1340);
    f.rx(cipher(f,plain,9005),1350); // Wrong-request authenticated RF IDs stay burned.
    f.rx(f.ack(201,false,meshtastic_Routing_Error_NO_RESPONSE),1360);f.poll(1361);
    assert(!replies(f.usb)&&!replies(f.ble)&&f.s.endpointFree()==3&&!f.s.storageStopSafe());
    f.rx(cipher(f,plain,9001),1400);f.poll(1401);delivered(f,f.usb,plain,9001);delivered(f,f.ble,plain,9001);
  }
  pass("bad AEAD unknown peer malformed inner wrong kind request and unsigned NAK cannot consume Neighbor query; authenticated wrong RF ID cannot be recycled");

  {Fixture f;const auto plain=response(f.peer.binding.node,201);f.rx(cipher(f,plain,9001),90);
    auto r=request(f);input(f.c,r,100);input(f.bc,r,101);f.rx(cipher(f,plain,9002),150);
    assert(!replies(f.usb)&&!replies(f.ble));f.poll(1000);f.complete(1200);
    f.rx(cipher(f,plain,9002),1300);assert(!replies(f.usb)&&!replies(f.ble)&&f.s.endpointFree()==3);
    f.rx(cipher(f,plain,9003),1400);f.poll(1401);delivered(f,f.usb,plain,9003);delivered(f,f.ble,plain,9003);
  }
  pass("unsolicited and pretransmit private responses never acquire later delivery approval");

  for(bool proof:{false,true}){Fixture f;originate(f);
    if(proof){f.rx(f.ack(201),1300);f.poll(1301);assert(f.s.endpointFree()==3);}
    f.rx(cipher(f,response(f.peer.binding.node,201),9001),90100);f.poll(90101);
    assert(!replies(f.usb)&&!replies(f.ble)&&f.s.endpointFree()==4&&f.s.storageStopSafe());
    f.rx(cipher(f,response(f.peer.binding.node,201),9002),90200);assert(!replies(f.usb)&&!replies(f.ble));
  }
  pass("absolute pending expiry rejects late Neighbor response after optional successful proof");

  {Fixture f;originate(f);f.c.resetStream();const auto plain=response(f.peer.binding.node,201);
    f.rx(cipher(f,plain,9001),1500);f.poll(1501);delivered(f,f.usb,plain,9001,0);delivered(f,f.ble,plain,9001);
    assert(f.s.storageStopSafe()&&f.s.endpointFree()==4);
  }
  {Fixture f;originate(f);const auto writes=f.lm.writes;assert(!f.s.revokePrivatePeer(f.peer.binding.node)&&f.lm.writes==writes);
    f.c.resetStream();f.bc.resetStream();f.poll(1300);assert(f.s.storageStopSafe()&&f.s.endpointFree()==4);
    assert(f.s.revokePrivatePeer(f.peer.binding.node)&&!f.runtime.ready(f.local.binding.node));
    f.rx(cipher(f,response(f.peer.binding.node,201),9001),1500);f.poll(1501);
    assert(!replies(f.usb)&&!replies(f.ble)&&f.radio.frames.size()==1);
  }
  pass("owner disconnect preserves remaining query owner; final disconnect releases pending session for quiescent peer revoke and rejects late reply");

  {Fixture f;originate(f);f.rx(f.ack(201),1300);f.poll(1301);f.runtime.freeze();f.poll(1400);
    assert(f.s.endpointFree()==4&&f.s.storageStopSafe());f.rx(cipher(f,response(f.peer.binding.node,201),9001),1500);
    assert(!replies(f.usb)&&!replies(f.ble)&&f.radio.frames.size()==1);
  }
  pass("private authority freeze cancels Neighbor application phase without Public fallback");

  for(bool populated:{false,true}){Fixture f;Snapshot producer{&f.s,snapshot(f.local.binding.node,populated)};bind(f,producer);
    const auto config=f.s.config;const auto wire=cipher(f,query(),9001);f.rx(wire,1000);
    assert(producer.calls==1&&f.s.receiving()&&!f.s.faulted()&&f.s.endpointControls()==1);
    f.poll(2000);assert(f.radio.frames.size()==1);f.complete(2200);const auto &reply=f.radio.frames[0];
    const auto expected=response(f.local.binding.node,9001,populated);
    assert(!reply[13]&&!(reply[12]&8)&&bench::le32(reply.data())==f.peer.binding.node&&peerDecrypt(f,reply)==expected);
    const auto data=decoded(expected);meshtastic_NeighborInfo info{};
    assert(mt::NeighborInfoPayload::decode(data.payload.bytes,data.payload.size,info)&&info.node_id==f.local.binding.node&&
      info.neighbors_count==unsigned(populated)&&info.node_broadcast_interval_secs==3600);
    const auto writes=f.lm.writes;f.rx(wire,2300);assert(producer.calls==1&&f.lm.writes==writes&&f.s.config.same(config));
  }
  pass("fresh RAM snapshot provider runs inside actual private storage pause and encrypts canonical empty or populated response once without settings mutation");

  {Fixture f;Snapshot producer{&f.s,snapshot(f.local.binding.node)};bind(f,producer);producer.available=false;
    f.rx(cipher(f,query(),9001),1000);assert(producer.calls==1&&!f.s.endpointControls()&&!f.entropy.calls);
    producer.available=true;f.rx(cipher(f,query(),9002),1001);
    assert(producer.calls==2&&f.s.endpointControls()==1&&f.entropy.calls==1);
    f.rx(cipher(f,query(),9003),181000);assert(producer.calls==2);
    // The first accepted reply, not the failed producer at1000, starts180s.
    f.rx(cipher(f,query(),9004),181001);assert(producer.calls==3);
  }
  pass("failed producer does not charge global Neighbor reply window; accepted queue closes provider until exact 180-second boundary");

  {Fixture f;Snapshot producer{&f.s,snapshot(f.local.binding.node)};bind(f,producer);
    for(uint32_t id=7001;id<7005;++id)f.rx(f.incoming(id,"fill-control",true),1000);
    assert(f.s.endpointControls()==4&&f.radio.frames.empty());f.rx(cipher(f,query(),9001),1100);
    assert(producer.calls==1&&f.s.endpointControls()==4);
    f.s.stop();assert(f.s.start(false,1200)&&f.s.armEndpoint(true));
    // The full queue burned a durable private TX ID before enqueue failed.
    // Repeating the deterministic random81 must not reuse that nonce/session ID.
    f.rx(cipher(f,query(),9002),1201);
    std::cout<<"DIAGNOSTIC full-control after fixed-random repeat calls="<<producer.calls<<" controls="<<f.s.endpointControls()<<" fault="<<f.s.faulted()<<'\n';
    assert(producer.calls==2&&!f.s.endpointControls()&&!f.s.faulted());
    const auto fresh=cipher(f,query(),9003);f.s.rx(fresh.data(),fresh.size(),3,1202,82);
    f.c.receive(fresh.data(),fresh.size(),-80,3,1202,mt::Companion::WorkspaceProof{true});f.bc.receive(fresh.data(),fresh.size(),-80,3,1202,mt::Companion::WorkspaceProof{true});
    assert(producer.calls==3&&f.s.endpointControls()==1);
  }
  pass("full actual private control queue does not charge Neighbor reply window; canceling controls permits a fresh query");

  for(bool privateFirst:{false,true}){Fixture f;Key other(61);Snapshot producer{&f.s,snapshot(f.local.binding.node)};bind(f,producer);
    const auto publicWire=publicQuery(f.s,other.binding.node,8001);
    producer.privateExpected=privateFirst;
    if(privateFirst)f.rx(cipher(f,query(),9001),1000);else f.rx(publicWire,1000);
    assert(producer.calls==1&&f.s.endpointControls()==1);producer.privateExpected=!privateFirst;
    if(privateFirst)f.rx(publicWire,1100);else f.rx(cipher(f,query(),9001),1100);
    assert(producer.calls==1&&f.s.endpointControls()==1);
  }
  pass("Public and authenticated private Neighbor replies share one postqueue window across unrelated sender IDs in both arrival orders");

  {Fixture f;f.s.stop();f.lm.onWrite={};Key other(61);
    assert(!f.ledger.addTrustedPeer(f.lm,other.binding.node,other.binding.publicKey,true));PkiPeerPin pins[2]{};
    pins[0].node=f.peer.binding.node;memcpy(pins[0].publicKey,f.peer.binding.publicKey,32);
    pins[1].node=other.binding.node;memcpy(pins[1].publicKey,other.binding.publicKey,32);PkiPeerSet set;
    assert(set.assign(f.local.binding,pins,2));MultiPeerRuntime multi(f.crypto,f.backend,f.identity,f.nonce,f.replay);
    assert(multi.open(f.local.binding.node,set));Radio radio;mt::Service service(radio,true,f.local.binding.node,81,true,false,&multi);
    f.gate.service=&service;f.lm.onWrite=[&]{assert(service.privateStoragePaused()&&!service.receiving()&&!radio.healthValue);};
    const auto config=f.s.config;assert(service.apply(config));Snapshot producer{&service,snapshot(f.local.binding.node)};
    assert(service.bindNeighborSnapshot(Snapshot::read,&producer)&&service.start(false,0)&&service.armEndpoint(true));
    const auto first=cipher(f,query(),9001),second=cipher(f,query(),9002,&other);
    service.rx(first.data(),first.size(),3,1000,81);assert(producer.calls==1&&service.endpointControls()==1);
    service.rx(second.data(),second.size(),3,1100,82);assert(producer.calls==1&&service.endpointControls()==1);
    assert(service.receiving()&&!service.faulted()&&multi.ready(f.local.binding.node)&&service.config.same(config));
    service.stop();f.lm.onWrite={};f.gate.service=&f.s;
  }
  pass("two real authenticated pinned peer sessions share the same Service Neighbor window without a per-peer bypass");
}
