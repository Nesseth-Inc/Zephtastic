// SPDX-License-Identifier: GPL-3.0-only
// Independent assertions against the selected implementation. Shared fixture
// provides only synthetic keys, memory storage, real PSA and mock radio/client.
#include "pki_runtime_fixture.h"
#include "NodeAdmin.h"
static void group(const char *s){std::cout<<"PASS "<<s<<std::endl;}
static Bytes proof(Fixture &f,const Key &signer,uint32_t from,uint32_t request,const Bytes &routing){
  uint8_t protectedWire[233];size_t n=0;mt::experimental::PkiAck ack(f.crypto);
  assert(ack.attach(signer.scalar,f.local.binding.publicKey,{true,from,f.local.binding.node,request},routing.data(),routing.size(),protectedWire,sizeof(protectedWire),n));
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_ROUTING_APP;d.request_id=request;d.has_bitfield=true;
  d.payload.size=n;memcpy(d.payload.bytes,protectedWire,n);uint8_t bytes[239],out[255];auto s=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&s,meshtastic_Data_fields,&d));
  size_t size=0;assert(f.s.codec.encodeZeroHopData(from,f.local.binding.node,request+1000,bytes,s.bytes_written,out,sizeof(out),size));return {out,out+size};
}
int main(){
  {Fixture f;auto wire=f.incoming(91,"auth",false);auto writes=f.lm.writes;
    for(size_t i=16;i<wire.size();++i){auto bad=wire;bad[i]^=1;f.rx(bad);assert(f.lm.writes==writes&&!f.delivered(f.usb)&&!f.delivered(f.ble));}
    for(unsigned i=0;i<12;++i){auto bad=wire;bad[i]^=0x80;f.rx(bad);assert(f.lm.writes==writes&&!f.delivered(f.usb));}
    auto bad=wire;bad[13]=1;f.rx(bad);assert(f.lm.writes==writes&&!f.delivered(f.usb));
    for(size_t n=0;n<wire.size();++n){f.rx(Bytes(wire.begin(),wire.begin()+n));assert(f.lm.writes==writes&&!f.delivered(f.usb));}
    f.rx(wire);assert(f.lm.writes==writes+1&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
    f.poll(2000);assert(!f.radio.sends);group("all ciphertext/tag/extra mutations and protected header changes fail without replay poisoning");}
  {Fixture f;Key third(155);f.send();f.poll();f.complete();auto writes=f.lm.writes;
    auto pending=[&]{f.poll(1700);assert(f.s.endpointFree()==3&&!f.result(10,true)&&!f.result(10,false)&&f.lm.writes==writes);};
    f.rx(f.ack(10,false));pending();
    f.rx(proof(f,third,third.binding.node,10,{24,0}));pending();
    f.rx(proof(f,third,f.peer.binding.node,10,{24,0}));pending();
    f.rx(f.ack(11));pending();
    f.rx(proof(f,f.peer,f.peer.binding.node,10,{10,0}));pending(); // authenticated route-discovery is not an error result
    f.rx(proof(f,f.peer,f.peer.binding.node,10,{24,33,80,77}));f.poll(1800);
    assert(f.result(10,true,33)==1&&!f.result(10,true,0)&&f.s.endpointFree()==4);
    f.rx(f.ack());f.poll(1900);assert(f.result(10,true,33)==1&&!f.result(10,true,0));
    group("third-peer pairwise MAC, missing proof, wrong correlation and non-error routing cannot complete; exact covered NAK survives");}
  {Fixture f;f.send();f.poll();f.rx(f.ack());assert(!f.result(10,true));assert(f.identity.close());f.complete();
    assert(!f.result(10,true)&&f.result(10,false,4)==1&&f.s.endpointFree()==4);group("early authenticated completion cannot survive identity revocation before native completion");}
  {Fixture f(true);auto bad=f.incoming(99,"bad tag",false);bad[16]^=1;f.rx(bad);assert(f.backend.real.healthy()&&f.runtime.ready(f.local.binding.node));
    f.send();f.poll();f.rx(f.ack());assert(!f.result(10,true));f.backend.real.close();uint8_t key[32]{};
    assert(!f.runtime.ready(f.local.binding.node)&&!f.s.privatePublicIdentity(key));f.complete();
    assert(!f.result(10,true)&&f.result(10,false,4)==1&&f.s.endpointFree()==4);
    PkiRuntime fresh(f.crypto,f.backend,f.identity,f.nonce,f.replay,&f.signing);assert(!fresh.open(f.local.binding.node,f.peer.binding.node));
    group("ordinary bad tag preserves crypto health but closed backend immediately revokes capability pending success and fresh open");}
  {Fixture f;assert(f.s.armEndpoint(false));auto writes=f.lm.writes;auto entropy=f.entropy.calls;f.send();
    assert(f.result(10,false,4)==1&&f.lm.writes==writes&&f.entropy.calls==entropy&&!f.radio.sends);
    f.rx(f.incoming(92,"receive only",true));assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1&&f.lm.writes==writes+1);
    f.poll(2000);assert(!f.radio.sends&&f.s.endpointFree()==4&&!f.s.endpointArmed());
    group("disarmed sender rejects before storage or entropy; authenticated receive delivers without control RF");}
  {Fixture f;f.s.stop();Radio radio;mt::Service service(radio,false,f.local.binding.node,81,true,false,&f.runtime);
    f.gate.service=&service;f.lm.onWrite=[&]{assert(service.privateStoragePaused()&&!radio.healthValue);};
    assert(service.apply(f.s.config)&&service.start(false,0)&&!service.armEndpoint(true));auto writes=f.lm.writes;auto plain=data();
    assert(service.submitPrivate(0,f.peer.binding.node,10,plain.data(),plain.size(),3,true,nullptr,0,false,100)<0);
    assert(f.lm.writes==writes&&!f.entropy.calls);auto wire=f.incoming(97,"compiled RX only",true);
    service.rx(wire.data(),wire.size(),3,1000,81);PrivateEvent event{};assert(service.takePrivateEvent(0,wire.data(),wire.size(),event));
    assert(f.lm.writes==writes+1&&service.endpointFree()==4);service.poll(2000,81);assert(!radio.sends&&!service.endpointArmed());
    group("compiled RX-only profile cannot arm or queue private TX and authenticated receive never generates ACK");}
  {Fixture f;Key stranger(155);auto writes=f.lm.writes;auto entropy=f.entropy.calls,signs=f.signing.calls,encrypts=f.backend.encrypts;
    auto unknown=[&](uint32_t id,meshtastic_ToRadio r){r.packet.id=id;r.packet.to=stranger.binding.node;input(f.c,r);
      assert(f.result(id,false,meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY)==1);
      assert(f.lm.writes==writes&&f.entropy.calls==entropy&&f.signing.calls==signs&&f.backend.encrypts==encrypts&&
        f.s.endpointFree()==4&&!f.radio.sends&&f.radio.frames.empty());};
    unknown(10,f.request(10));
    auto hinted=f.request(16);hinted.packet.public_key.size=31;memset(hinted.packet.public_key.bytes,0x5a,31);unknown(16,hinted);
    auto explicitMissing=hinted;explicitMissing.packet.pki_encrypted=true;unknown(17,explicitMissing);
    auto telemetry=f.request(13);telemetry.packet.from=0;telemetry.packet.want_ack=false;telemetry.packet.decoded={};telemetry.packet.decoded.portnum=meshtastic_PortNum_TELEMETRY_APP;
    telemetry.packet.decoded.want_response=true;telemetry.packet.decoded.payload.size=2;telemetry.packet.decoded.payload.bytes[0]=0x12;telemetry.packet.decoded.payload.bytes[1]=0;
    unknown(13,telemetry);
    auto neighbor=f.request(14);neighbor.packet.from=0;neighbor.packet.want_ack=false;neighbor.packet.decoded={};neighbor.packet.decoded.portnum=meshtastic_PortNum_NEIGHBORINFO_APP;
    neighbor.packet.decoded.want_response=true;
    unknown(14,neighbor);
    auto application=f.request(15);application.packet.from=0;application.packet.want_ack=false;const uint8_t appPayload[]={'a','p','p','!'};uint8_t appData[64]{};size_t appSize=0;
    assert(mt::PublicApplication::encode(256,appPayload,sizeof(appPayload),23,0x1f44d,appData,sizeof(appData),appSize));
    auto appIn=pb_istream_from_buffer(appData,appSize);application.packet.decoded={};
    assert(pb_decode(&appIn,meshtastic_Data_fields,&application.packet.decoded)&&!appIn.bytes_left);
    unknown(15,application);
    auto r=f.request(11);r.packet.pki_encrypted=true;r.packet.public_key.size=31;input(f.c,r);assert(f.result(11,false,34)==1);
    assert(!f.entropy.calls&&f.s.endpointFree()==4&&!f.radio.sends);
    f.runtime.freeze();f.send(12);f.poll();assert(f.result(12,false,34)==1&&!f.radio.sends&&f.s.endpointFree()==4);
    group("unknown active-private-key destinations fail closed for text, telemetry, neighbor and generic application without channel encode; malformed hints and frozen runtime retain PKI error");}
  for(unsigned mode=0;mode<5;++mode){Fixture f(true,true);auto r=f.request(410+mode);auto &p=r.packet;p.decoded={};p.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;
    if(mode==0)p.pki_encrypted=true;
    if(mode==1){p.pki_encrypted=true;p.public_key.size=32;memcpy(p.public_key.bytes,f.peer.binding.publicKey,32);}
    if(mode==2){p.pki_encrypted=true;p.public_key.size=31;p.public_key.bytes[0]=1;}
    if(mode==3){p.decoded.xeddsa_signature.size=64;memset(p.decoded.xeddsa_signature.bytes,1,64);}
    if(mode==4){p.decoded.payload.size=1;p.decoded.payload.bytes[0]=0;}
    auto writes=f.lm.writes;input(f.c,r);assert(f.result(410+mode,false,mode<3?34:32)==1);
    assert(f.lm.writes==writes&&!f.signing.calls&&!f.entropy.calls&&!f.radio.sends&&f.s.endpointFree()==4);
  }group("direct NodeInfo dispatch preserves explicit-PKI PKI_FAILED versus malformed-public BAD_REQUEST before any admission");
  for(size_t hintLength:{size_t(1),size_t(31),size_t(32)}){Fixture f(true,true);auto r=f.request(420+uint32_t(hintLength));
    auto &p=r.packet;p.from=0;p.want_ack=false;p.decoded={};p.decoded.portnum=meshtastic_PortNum_NODEINFO_APP;
    p.public_key.size=hintLength;memset(p.public_key.bytes,0x5a,hintLength);input(f.c,r);f.poll();
    assert(f.radio.frames.size()==1&&f.signing.calls==1&&!f.backend.encrypts&&f.entropy.calls==1);
    const auto &wire=f.radio.frames[0];bench::RouteMetadata metadata{};
    assert(f.s.codec.decode(wire.data(),wire.size(),metadata)&&metadata.portnum==meshtastic_PortNum_NODEINFO_APP&&
      metadata.signatureLength==64&&bench::le32(wire.data())==f.peer.binding.node&&bench::le32(wire.data()+4)==f.local.binding.node);
    bench::MtSignature verifier;assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,1000));
    assert(verifier.apply(wire.data(),wire.size(),metadata,1000)==bench::MtSignature::Result::Verified);
  }group("NodeInfo ignores bounded hint-only fields and emits real verified signed channel data without private CCM");
  for(int mode=1;mode<=2;++mode){Fixture f;auto wire=f.incoming(93,"persist first",false);auto writes=f.lm.writes;f.lm.fault=mode;f.rx(wire);
    assert(f.s.faulted()&&!f.s.receiving()&&!f.s.endpointArmed()&&!f.runtime.ready(f.local.binding.node));
    assert(f.lm.writes==writes+1&&!f.delivered(f.usb)&&!f.delivered(f.ble)&&!f.radio.sends);f.lm.fault=0;
    PkiLedger reboot;assert(!reboot.load(f.lm));assert(reboot.seen(f.peer.binding.node,f.peer.binding.publicKey,93)==(mode==1?ReplayCheck::Fresh:ReplayCheck::Seen));
    f.rx(wire);assert(f.lm.writes==writes+1&&!f.delivered(f.usb));
  }group("before/after durable replay write errors freeze without plaintext or blind retry and restart reflects actual persistence");
  // Storage resumes the existing RX session through restore(), not start().
  {Fixture f;auto wire=f.incoming(94,"restore failure",true);f.radio.restoreError=-EIO;f.rx(wire);
    assert(f.s.faulted()&&!f.s.receiving()&&!f.s.endpointArmed()&&!f.runtime.ready(f.local.binding.node)&&
           !f.delivered(f.usb)&&!f.delivered(f.ble)&&!f.radio.sends&&f.s.endpointFree()==4);
    PkiLedger reboot;assert(!reboot.load(f.lm)&&reboot.seen(f.peer.binding.node,f.peer.binding.publicKey,94)==ReplayCheck::Seen);
    group("post-commit radio restore failure burns replay and cancels ACK without publishing plaintext");}
  {Fixture f;auto wire=f.incoming(95,"across reset",false);f.rx(wire);assert(f.delivered(f.usb)==1);f.send(44);assert(f.ledger.sent(44));
    f.s.stop();f.lm.onWrite=nullptr;assert(f.identity.close());
    MonocypherIdentityDeriver deriver;IdentityJournal identity;assert(!identity.load(f.im,deriver));PkiLedger ledger;assert(!ledger.load(f.lm));
    Gate gate;Entropy entropy;LedgerNonceAuthority nonce(identity,ledger,f.lm,entropy,gate);LedgerReplayAuthority replay(identity,ledger,f.lm,gate);
    PkiRuntime runtime(f.crypto,f.backend,identity,nonce,replay);assert(runtime.open(f.local.binding.node,f.peer.binding.node));
    Radio radio;mt::Service service(radio,true,f.local.binding.node,81,true,false,&runtime);gate.service=&service;
    assert(service.apply(f.s.config)&&service.start(false,0)&&service.armEndpoint(true));auto writes=f.lm.writes;auto decrypts=f.backend.decrypts;
    service.rx(wire.data(),wire.size(),3,1000,81);PrivateEvent event{};assert(!service.takePrivateEvent(0,wire.data(),wire.size(),event));
    assert(f.backend.decrypts==decrypts+1&&f.lm.writes==writes&&service.endpointFree()==4);
    auto plain=data();assert(service.submitPrivate(0,f.peer.binding.node,44,plain.data(),plain.size(),3,true,nullptr,0,false,1200)<0);
    assert(!entropy.calls&&f.lm.writes==writes&&service.endpointFree()==4&&!radio.sends);
    assert(!service.submitPrivate(0,f.peer.binding.node,45,plain.data(),plain.size(),3,true,nullptr,0,false,1300));
    assert(entropy.calls==1&&ledger.sent(45));group("fresh runtime and identity objects reject replay and outgoing ID reuse across simulated reset");}
  {Fixture f;f.send();f.poll();Bytes first=f.radio.frames.at(0);f.complete();
    for(unsigned i=0;i<2;++i){uint32_t now=20000+i*20000;f.poll(now);f.poll(now+2000);assert(f.radio.frames.size()==i+2&&f.radio.frames.back()==first);f.complete(now+2200);}
    f.poll(100000);assert(f.radio.frames.size()==3&&f.entropy.calls==1&&f.result(10,false,3)==1&&f.s.endpointFree()==4);
    group("three bounded attempts reuse exact ciphertext and exhaust with one unauthenticated timeout");}
  {Fixture f;Memory settings;mt::NodeJournal journal;assert(!journal.load(settings,f.s.nodes,f.local.binding.node));assert(f.s.armEndpoint(false));
    settings.fault=1;assert(mt::NodeAdmin::edit(f.s,f.radio,journal,settings,f.peer.binding.node,mt::NodeCache::Edit::Ignore,1000)<0);
    assert(!f.s.receiving()&&!f.s.endpointArmed()&&f.runtime.ready(f.local.binding.node));
    PkiLedger reboot;assert(!reboot.load(f.lm)&&reboot.active(f.peer.binding.node,f.peer.binding.publicKey));
    uint8_t key[32]{};assert(reboot.lookup(f.peer.binding.node,key)&&!memcmp(key,f.peer.binding.publicKey,32));
    group("failed reversible Ignore policy write holds radio without retiring the trusted key domain");}
  {Fixture f;Memory settings;mt::NodeJournal journal;assert(!journal.load(settings,f.s.nodes,f.local.binding.node));assert(f.s.armEndpoint(false));
    assert(!mt::NodeAdmin::edit(f.s,f.radio,journal,settings,f.peer.binding.node,mt::NodeCache::Edit::Ignore,1000));
    auto writes=f.lm.writes;assert(f.s.receiving()&&!f.s.endpointArmed()&&f.runtime.ready(f.local.binding.node));
    assert(!mt::NodeAdmin::edit(f.s,f.radio,journal,settings,f.peer.binding.node,mt::NodeCache::Edit::Unignore,1050));
    assert(f.lm.writes==writes&&f.runtime.ready(f.local.binding.node)&&!f.s.nodes.ignored(f.peer.binding.node));
    assert(!mt::NodeAdmin::edit(f.s,f.radio,journal,settings,f.peer.binding.node,mt::NodeCache::Edit::Remove,1100));
    assert(f.lm.writes==writes+1&&!f.s.receiving()&&!f.s.endpointArmed()&&!f.runtime.ready(f.local.binding.node)&&!f.s.faulted());
    ++writes;
    assert(!mt::NodeAdmin::edit(f.s,f.radio,journal,settings,f.peer.binding.node,mt::NodeCache::Edit::Remove,1200));
    assert(f.lm.writes==writes);group("Ignore and Unignore retain exact trust history; explicit Remove commits one permanent tombstone");}
  for(unsigned mode=0;mode<4;++mode){Fixture f(true,true);auto writes=f.lm.writes;
    for(uint32_t id:{201u,202u}){bench::Frame query;assert(mt::Discovery::encode(f.s.codec,f.s.config.node(),f.peer.binding.node,81,f.local.binding.node,id,true,0,query));f.rx(Bytes(query.bytes,query.bytes+query.len),1500);}
    assert(!f.signing.calls&&f.lm.writes==writes&&!f.radio.sends);
    if(mode==0){f.poll(2000);f.poll(2500);assert(f.signing.calls==1&&f.lm.writes==writes+1&&f.radio.frames.size()==1);
      bench::RouteMetadata m;auto &wire=f.radio.frames[0];assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.request_id==201&&m.signatureLength==64);}
    else {
      if(mode==1){f.radio.busyValue=true;f.poll(2000);f.poll(7000);f.radio.busyValue=false;}
      if(mode==2)assert(f.identity.close());
      if(mode==3)assert(f.s.armEndpoint(false));
      f.poll(8000);assert(!f.signing.calls&&f.lm.writes==writes&&!f.radio.sends);
    }
  }group("one deferred signed discovery slot retains first request and expires or cancels before signing on busy revoke or disarm");
  {Fixture f;Memory settings;mt::NodeJournal journal;assert(!journal.load(settings,f.s.nodes,f.local.binding.node));assert(f.s.armEndpoint(false));
    assert(!mt::NodeAdmin::edit(f.s,f.radio,journal,settings,f.peer.binding.node,mt::NodeCache::Edit::Remove,1000));
    f.s.stop();f.lm.onWrite=nullptr;assert(f.identity.close());
    MonocypherIdentityDeriver deriver;IdentityJournal identity;assert(!identity.load(f.im,deriver));PkiLedger ledger;assert(!ledger.load(f.lm));
    Gate gate;Entropy entropy;LedgerNonceAuthority nonce(identity,ledger,f.lm,entropy,gate);LedgerReplayAuthority replay(identity,ledger,f.lm,gate);
    PkiRuntime runtime(f.crypto,f.backend,identity,nonce,replay);assert(!runtime.open(f.local.binding.node,f.peer.binding.node));
    Radio radio;mt::Service service(radio,true,f.local.binding.node,81,true,false,&runtime);gate.service=&service;mt::NodeJournal reloaded;
    assert(!reloaded.load(settings,service.nodes,f.local.binding.node)&&service.nodes.find(f.peer.binding.node)==-1);
    assert(service.apply(f.s.config)&&!service.start(false,0)&&!service.receiving());auto writes=f.lm.writes;
    int ret=mt::NodeAdmin::edit(service,radio,reloaded,settings,f.peer.binding.node,mt::NodeCache::Edit::Remove,1100);
    if(ret)std::cerr<<"restart tombstone removal returned "<<ret<<" fault="<<service.faulted()<<std::endl;
    assert(!ret&&!service.faulted()&&service.nodes.count()==0&&f.lm.writes==writes&&!runtime.ready(f.local.binding.node));
    assert(!service.receiving()&&!service.endpointArmed()&&!radio.sends);
    assert(ledger.revoked(f.peer.binding.node)&&replay.revoked(f.peer.binding.node)&&!replay.revoked(f.local.binding.node));
    Memory absent;PkiLedger empty;assert(!empty.load(absent)&&!empty.revoked(f.peer.binding.node));
    LedgerReplayAuthority missing(identity,empty,absent,gate);assert(!missing.revoked(f.peer.binding.node));
    Memory corrupt;corrupt.values=f.lm.values;corrupt.values.begin()->second[0]^=1;PkiLedger broken;
    assert(broken.load(corrupt)<0&&!broken.revoked(f.peer.binding.node));
    LedgerReplayAuthority damaged(identity,broken,corrupt,gate);assert(!damaged.revoked(f.peer.binding.node));
    Memory otherMemory;IdentityJournal otherIdentity;assert(!otherIdentity.load(otherMemory,deriver));IdentityCandidate other(f.peer.binding,f.peer.scalar);
    assert(!otherIdentity.store(otherMemory,deriver,other,true));LedgerReplayAuthority unbound(otherIdentity,ledger,f.lm,gate);assert(!unbound.revoked(f.peer.binding.node));
    assert(identity.close()&&!replay.revoked(f.peer.binding.node));ledger.close();assert(!ledger.revoked(f.peer.binding.node)&&f.lm.writes==writes);
    group("cache cleanup after tombstone and fresh runtime reset remains possible without private trust or ledger rewrite");}
  {Fixture f;Key third(155);auto nodeInfo=[&](const uint8_t key[32],uint32_t id){
      meshtastic_User user{};strcpy(user.long_name,"Untrusted advertisement");strcpy(user.short_name,"PUB");
      user.public_key.size=32;memcpy(user.public_key.bytes,key,32);meshtastic_Data d{};d.portnum=meshtastic_PortNum_NODEINFO_APP;d.has_bitfield=true;
      auto u=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&u,meshtastic_User_fields,&user));d.payload.size=u.bytes_written;
      uint8_t plain[239],wire[255];auto s=pb_ostream_from_buffer(plain,sizeof(plain));assert(pb_encode(&s,meshtastic_Data_fields,&d));size_t n=0;
      assert(f.s.codec.encodeZeroHopData(f.peer.binding.node,f.local.binding.node,id,plain,s.bytes_written,wire,sizeof(wire),n));return Bytes(wire,wire+n);
    };
    auto writes=f.lm.writes;f.rx(nodeInfo(third.binding.publicKey,300));assert(f.usb.out.empty()&&f.ble.out.empty()&&f.s.nodes.count()==0&&f.lm.writes==writes);
    f.s.signature.clear();f.rx(nodeInfo(third.binding.publicKey,301));assert(f.usb.out.empty()&&f.ble.out.empty()&&f.s.nodes.count()==0);
    uint8_t trusted[32];assert(f.replay.lookup(f.peer.binding.node,trusted)&&!memcmp(trusted,f.peer.binding.publicKey,32));
    f.rx(f.incoming(96,"original pin",false));assert(f.delivered(f.usb)==1);
    group("wire-level conflicting advertised key cannot replace pin or reach clients across signature cache reset");}
  {Fixture f;auto r=f.request(401);auto &p=r.packet;p.to=f.local.binding.node;p.decoded={};p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    p.pki_encrypted=true;p.public_key.size=32;memset(p.public_key.bytes,0xA5,32);p.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;
    p.decoded.xeddsa_signature.size=64;memset(p.decoded.xeddsa_signature.bytes,0x5A,64);
    meshtastic_AdminMessage request{};request.which_payload_variant=meshtastic_AdminMessage_get_device_connection_status_request_tag;request.get_device_connection_status_request=true;
    auto b=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&b,meshtastic_AdminMessage_fields,&request));p.decoded.payload.size=b.bytes_written;
    auto writes=f.lm.writes;input(f.c,r);unsigned replies=0;for(const auto &out:f.usb.out)if(out.which_payload_variant==meshtastic_FromRadio_packet_tag&&out.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP){
      assert(!out.packet.pki_encrypted&&!out.packet.public_key.size&&!out.packet.decoded.xeddsa_signature.size&&out.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT);
      assert(out.packet.from==f.local.binding.node&&out.packet.decoded.request_id==401);meshtastic_AdminMessage reply{};auto s=pb_istream_from_buffer(out.packet.decoded.payload.bytes,out.packet.decoded.payload.size);assert(pb_decode(&s,meshtastic_AdminMessage_fields,&reply));assert(reply.which_payload_variant==meshtastic_AdminMessage_get_device_connection_status_response_tag);++replies;
    }
    assert(replies==1&&f.lm.writes==writes&&!f.entropy.calls&&!f.radio.sends&&!f.delivered(f.usb));
    group("stock-shaped local admin with forged key/proof hints yields fresh unclaimed local reply and no private admission");}
}
