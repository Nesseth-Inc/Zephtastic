#include "pki_runtime_fixture.h"
static void group(const char *s){std::cout<<"PASS "<<s<<"\n";}
int main(){
  // Qwen slot4 proposal intent, corrected by root: actual Frame and metadata
  // types, packet-ID header correlation, and PASS only after assertions.
  {Fixture f;assert(f.s.beginPrivateAdministration());const auto entropy=f.entropy.calls;
    constexpr uint8_t query[]={8,3,24,1,72,2};bench::Frame out;
    assert(f.runtime.encodeChannelControl(f.s.codec,f.peer.binding.node,41,query,sizeof(query),out));
    bench::RouteMetadata m;
    assert(f.s.codec.decode(out.bytes,out.len,m)&&m.channelDecoded&&m.portnum==3&&m.wantResponse&&
      !m.request_id&&m.hasBitfield&&m.bitfield==2&&bench::le32(out.bytes+8)==41);
    assert(f.ledger.sent(41)&&f.entropy.calls==entropy+1&&f.s.endPrivateAdministration());
    group("runtime canonical position query uses exact codec shape and one outgoing-ID reservation");}
  {Fixture f;assert(f.s.beginPrivateAdministration());f.entropy.fail=true;const auto entropy=f.entropy.calls;
    constexpr uint8_t query[]={8,3,24,1,72,2};bench::Frame out;
    memset(out.bytes,0xee,sizeof(out.bytes));out.len=123;out.dueAt=1;out.expires=2;out.used=true;out.attempts=3;
    const auto before=out;
    assert(!f.runtime.encodeChannelControl(f.s.codec,f.peer.binding.node,42,query,sizeof(query),out));
    assert(f.entropy.calls==entropy+1&&!memcmp(out.bytes,before.bytes,sizeof(out.bytes))&&out.len==before.len&&
      out.dueAt==before.dueAt&&out.expires==before.expires&&out.used==before.used&&out.attempts==before.attempts);
    assert(f.s.endPrivateAdministration());
    group("failed direct control reservation leaves all caller frame fields unchanged and resumes RX");}
  {Fixture f;mt::NodeCache::Entry peer{};peer.identity(f.peer.binding.node,81,0);
   assert(peer.text(peer.bytes+peer.Name,40,"Synthetic private peer",23)&&peer.text(peer.bytes+peer.Short,5,"PRV",4));
   assert(f.s.nodes.accept(peer,f.local.binding.node,1,0)>=0&&f.s.nodes.edit(f.peer.binding.node,f.local.binding.node,mt::NodeCache::Edit::ToggleMute)==1);
   auto frame=f.incoming(89,"muted private still delivered",false);f.rx(frame,1400);
   assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1&&f.usb.previews==1&&f.ble.previews==1&&
     f.usb.previewFrom==f.peer.binding.node&&f.usb.previewTo==f.local.binding.node&&f.usb.previewId==89&&
     f.usb.previewPort==meshtastic_PortNum_TEXT_MESSAGE_APP&&!f.usb.previewLocal&&!f.radio.sends);
   group("authenticated private text retains both-client delivery and supplies real sender/destination to shared mute-attention boundary");}
  {Fixture f(true);auto frame=f.incoming();frame[16]^=1;f.rx(frame);
    uint8_t pub[32];assert(f.backend.healthy()&&f.runtime.ready(f.local.binding.node)&&f.s.privatePublicIdentity(pub));
    f.send();f.poll();f.rx(f.ack());assert(!f.result(10,true));assert(f.backend.real.close());
    assert(!f.runtime.ready(f.local.binding.node)&&!f.s.privatePublicIdentity(pub));
    f.complete();assert(!f.result(10,true)&&f.result(10,false,4)==1);
    PkiRuntime fresh(f.crypto,f.backend,f.identity,f.nonce,f.replay);
    assert(!fresh.open(f.local.binding.node,f.peer.binding.node)&&!fresh.ready(f.local.binding.node));
    group("bad tag preserves readiness; closed crypto removes capability and pending success and refuses open");}
  {Fixture f;f.send();assert(f.entropy.calls==1&&f.ledger.sent(10)&&f.s.endpointFree()==3&&f.s.receiving()&&f.s.endpointArmed());f.poll();assert(f.radio.sends==1);auto &wire=f.radio.frames[0];assert(!wire[13]&&bench::le32(wire.data()+4)==f.local.binding.node);uint8_t plain[227];size_t n;assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,10,wire.data()+16,wire.size()-16,plain,227,n));auto expected=data();assert(n==expected.size()&&!memcmp(plain,expected.data(),n));f.complete();f.rx(f.ack(10,false));f.poll(1501);assert(!f.result(10,true)&&!f.result(10,false));f.rx(f.ack());f.poll(1600);assert(f.result(10,true,0)==1&&f.s.endpointFree()==4);group("stock-shaped sendText auto selects private; exact decryption and correlated proof completion");}
  {Fixture f;f.send();auto encrypts=f.backend.encrypts;auto r=f.request();input(f.bc,r);assert(f.entropy.calls==1&&f.backend.encrypts==encrypts);r.packet.decoded.payload.bytes[0]^=1;input(f.c,r);assert(f.entropy.calls==1&&f.s.endpointFree()==3);f.poll();auto first=f.radio.frames[0];f.complete();f.poll(20000);f.poll(22000);assert(f.radio.frames.size()==2&&f.radio.frames[1]==first&&f.entropy.calls==1&&f.backend.encrypts==encrypts);group("USB BLE merge and retries preserve exact ciphertext; changed plaintext rejected");}
  {Fixture f;auto r=f.request();r.packet.pki_encrypted=true;input(f.c,r);assert(f.entropy.calls==1);r=f.request(11);r.packet.pki_encrypted=true;r.packet.public_key.size=32;memset(r.packet.public_key.bytes,1,32);input(f.c,r);assert(f.entropy.calls==1&&f.result(11,false,34)==1);r=f.request(12);r.packet.to=0xffffffffu;r.packet.pki_encrypted=true;input(f.c,r);assert(f.entropy.calls==1&&f.result(12,false,34)==1);group("explicit private stored-key selection and explicit wrong-key/broadcast refusal");}
  for(uint8_t hintSize:{uint8_t(31),uint8_t(32)}) {Fixture f;auto r=f.request(20+hintSize);r.packet.public_key.size=hintSize;memset(r.packet.public_key.bytes,1,hintSize);input(f.c,r);
    assert(f.entropy.calls==1&&f.ledger.sent(20+hintSize)&&f.s.endpointFree()==3&&f.s.receiving()&&f.s.endpointArmed());
    f.poll();assert(f.radio.frames.size()==1);const auto &wire=f.radio.frames[0];assert(!wire[13]&&bench::le32(wire.data()+4)==f.local.binding.node&&bench::le32(wire.data()+8)==20+hintSize);
    uint8_t plain[227];size_t n;assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,20+hintSize,wire.data()+16,wire.size()-16,plain,227,n));
    auto expected=data();assert(n==expected.size()&&!memcmp(plain,expected.data(),n));
    f.complete();f.rx(f.ack(20+hintSize,false));f.poll(1501);assert(!f.result(20+hintSize,true)&&!f.result(20+hintSize,false));
    f.rx(f.ack(20+hintSize));f.poll(1600);assert(f.result(20+hintSize,true,0)==1&&f.s.endpointFree()==4);
  }
  group("non-PKI wrong-size and short public-key hints are ignored; pinned PSA key encrypts canonical text");
  {Fixture f;auto frame=f.incoming();const auto decrypts=f.backend.decrypts;const auto writes=f.lm.writes;f.rx(frame);assert(f.backend.decrypts==decrypts+1&&f.lm.writes==writes+2&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1);assert(f.hot.calls==1&&f.hot.from==f.peer.binding.node&&f.hot.utc==1792660001u&&f.hot.uptime==1&&!f.hot.viaMqtt);f.c.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});assert(f.delivered(f.usb)==1);f.rx(frame,1600);assert(f.backend.decrypts==decrypts+2&&f.lm.writes==writes+3&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1&&f.hot.calls==1);f.poll(2000);assert(f.radio.sends==1);bench::RouteMetadata m;assert(f.s.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),m)&&m.ackProofLength==8);group("one RX commit and immutable USB BLE fanout; authenticated observation is one-shot across Seen duplicate");}
  {Fixture f;auto frame=f.incoming();auto bad=frame;bad[16]^=1;auto writes=f.lm.writes;f.rx(bad);assert(f.lm.writes==writes&&!f.delivered(f.usb)&&f.hot.calls==0);f.rx(frame);assert(f.delivered(f.usb)==1&&f.hot.calls==1);auto changed=f.incoming(90,"different",true);f.rx(changed);assert(f.delivered(f.usb)==1&&f.lm.writes==writes+2&&f.hot.calls==1);group("tamper cannot poison replay or metadata; authenticated reused ID is not observed twice");}
  {Fixture f;f.send();f.poll();f.rx(f.ack());assert(!f.result(10,true));assert(f.identity.close());f.complete();assert(!f.result(10,true)&&f.result(10,false,4)==1);group("early proof followed by identity revocation cannot survive native completion");}
  {Fixture f;auto r=f.request();r.packet.to=f.local.binding.node;r.packet.pki_encrypted=true;r.packet.public_key.size=32;memcpy(r.packet.public_key.bytes,f.peer.binding.publicKey,32);input(f.c,r);assert(!f.delivered(f.usb)&&f.result(10,false,34)==1&&f.entropy.calls==0);r.packet.pki_encrypted=false;r.packet.public_key.size=0;r.packet.ack_proof_status=meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID;r.packet.id=11;input(f.c,r);assert(f.result(11,false,34)==1);group("local echo rejects caller PKI publickey and proof authority");}
  {Fixture f;f.send(10);const auto before=f.lm.writes;f.radio.busyValue=true;f.send(11);assert(f.lm.writes==before&&f.s.endpointFree()==3);f.radio.busyValue=false;f.lm.onWrite=[&]{assert(f.s.privateStoragePaused());assert(!f.s.start(false,0)&&!f.s.apply(f.s.config)&&!f.s.armEndpoint(false)&&!f.s.armEndpoint(true));assert(f.s.endpointFree()==3);};f.send(12);assert(f.s.endpointFree()==2&&f.s.endpointArmed()&&f.radio.sends==0);group("busy fails before writes; durable pause retains queued work and blocks reentry");}
  for(unsigned mode=0;mode<6;++mode){Fixture f;auto writes=f.lm.writes;if(mode==0)f.radio.stopError=-EIO;if(mode==1)f.radio.stuckOn=true;if(mode==2)f.radio.restoreError=-EIO;if(mode==3)f.radio.stuckOff=true;if(mode==4)f.lm.fault=1;if(mode==5)f.lm.fault=2;f.send();assert(f.s.faulted()&&!f.s.receiving()&&!f.s.endpointArmed()&&f.radio.sends==0&&!f.runtime.ready(f.local.binding.node));assert(mode>1||f.lm.writes==writes);}group("stop health restore and uncertain journal failures freeze before RF continuation");
  {Fixture f;f.entropy.fail=true;f.send();assert(f.ledger.sent(10)&&f.entropy.calls==1&&!f.radio.sends&&f.s.endpointFree()==4);f.entropy.fail=false;f.send();assert(f.entropy.calls==1);f.send(11);assert(f.entropy.calls==2&&f.s.endpointFree()==3);group("entropy failure burns durable ID and cannot retry nonce; new IDs remain bounded");}
  {Fixture f;auto frame=f.incoming();f.radio.restoreError=-EIO;f.rx(frame);assert(f.ledger.seen(f.peer.binding.node,f.peer.binding.publicKey,90)==ReplayCheck::Seen&&!f.delivered(f.usb)&&!f.delivered(f.ble)&&f.s.faulted());group("successful replay commit with failed RF restore publishes no plaintext or ACK");}
  {Fixture f;assert(f.s.revokePrivatePeer(f.peer.binding.node));assert(f.s.revokePrivatePeer(f.peer.binding.node));assert(!f.s.receiving()&&!f.s.endpointArmed()&&!f.runtime.ready(f.local.binding.node));PkiLedger reboot;assert(!reboot.load(f.lm)&&!reboot.active(f.peer.binding.node,f.peer.binding.publicKey));group("explicit durable tombstone survives reboot and cannot revive runtime trust");}
  {Fixture f;f.send();f.poll();f.complete();f.rx(f.ack(10,true,33));f.poll(1600);assert(f.result(10,true,33)==1&&!f.result(10,true,0));group("authenticated NAK stays rejection and raw Routing never bypasses correlation");}

  {Fixture f(true,true);uint8_t pub[32];assert(f.s.privatePublicIdentity(pub)&&!memcmp(pub,f.local.binding.publicKey,32));
    assert(!f.s.submitNodeInfo(0,200,0xffffffffu,3,false,false,100));assert(f.ledger.sent(200)&&f.signing.calls==1);
    assert(!f.s.submitNodeInfo(1,200,0xffffffffu,3,false,false,101)&&f.signing.calls==1);
    f.poll(1000);assert(f.radio.frames.size()==1);bench::RouteMetadata m;auto &wire=f.radio.frames[0];
    assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.nodeInfoValid&&m.signatureLength==64&&m.publicKeyLength==32);
    bench::MtSignature stockGate;assert(stockGate.apply(wire.data(),wire.size(),m,1000)==bench::MtSignature::Result::Verified);
    assert(bench::MtSignature::nodeId(m.publicKey)==f.local.binding.node);group("signed NodeInfo bootstrap advertises exact identity and stock first-contact verification passes");}
  {Fixture f(true,true);f.signing.fail=true;assert(f.s.submitNodeInfo(0,200,0xffffffffu,3,false,false,100)<0&&f.ledger.sent(200)&&!f.radio.sends);
    f.signing.fail=false;assert(f.s.submitNodeInfo(0,200,0xffffffffu,3,false,false,101)<0);assert(!f.s.submitNodeInfo(0,201,0xffffffffu,3,false,false,102));
    group("signing entropy failure burns ID with no unsigned fallback");}
  {Fixture f(false,true);uint8_t pub[32];assert(!f.s.privatePublicIdentity(pub));assert(f.s.submitNodeInfo(0,200,0xffffffffu,3,false,false,100)<0&&!f.ledger.sent(200));
    group("pre-pinned runtime without signer never advertises PKC or unsigned bootstrap");}
  {Fixture f;f.lm.onWrite=[&]{f.s.stop();};f.send();assert(f.s.faulted()&&!f.s.endpointArmed()&&f.s.endpointFree()==4&&!f.runtime.ready(f.local.binding.node));
    group("unexpected transaction cancellation freezes and clears newly queued ciphertext");}

  {Fixture f;bench::RouteMetadata m;m.portnum=4;m.publicKeyLength=32;memcpy(m.publicKey,f.peer.binding.publicKey,32);
    assert(f.s.privatePeerMetadata(f.peer.binding.node,m));m.publicKey[7]^=1;assert(!f.s.privatePeerMetadata(f.peer.binding.node,m));
    f.s.signature.clear();assert(!f.s.privatePeerMetadata(f.peer.binding.node,m));group("exact pinned key collision guard survives signature cache expiry");}

  {Fixture f(true,true);bench::Frame query;assert(mt::Discovery::encode(f.s.codec,f.s.config.node(),f.peer.binding.node,81,f.local.binding.node,201,true,0,query));
    f.rx(Bytes(query.bytes,query.bytes+query.len));assert(!f.signing.calls&&!f.radio.sends);
    f.poll(2000);assert(f.signing.calls==1&&f.ledger.sent(71));f.poll(2500);assert(f.radio.sends==1);
    bench::RouteMetadata m;const auto &wire=f.radio.frames[0];assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.request_id==201&&m.signatureLength==64);
    group("signed automatic NodeInfo encoding is deferred outside RX metadata stack");}
  {Fixture f(true,true);bench::Frame query;assert(mt::Discovery::encode(f.s.codec,f.s.config.node(),f.peer.binding.node,81,f.local.binding.node,201,true,0,query));
    f.rx(Bytes(query.bytes,query.bytes+query.len));assert(f.s.armEndpoint(false));f.poll(2000);assert(!f.signing.calls&&!f.radio.sends);
    group("disarm cancels deferred signed bootstrap before entropy or durable writes");}

  {Fixture f;assert(f.s.revokePrivatePeer(f.peer.binding.node));
    IdentityJournal rebootIdentity;PkiLedger rebootLedger;assert(!rebootIdentity.load(f.im,f.deriver)&&!rebootLedger.load(f.lm));
    Gate gate;LedgerNonceAuthority nonce(rebootIdentity,rebootLedger,f.lm,f.entropy,gate);
    LedgerReplayAuthority replay(rebootIdentity,rebootLedger,f.lm,gate);
    PkiRuntime runtime(f.crypto,f.backend,rebootIdentity,nonce,replay);
    assert(!runtime.open(f.local.binding.node,f.peer.binding.node)&&replay.revoked(f.peer.binding.node));
    Radio radio;mt::Service service(radio,true,f.local.binding.node,81,true,false,&runtime);
    mt::test::KnownChannelFixtureClock serviceClock;serviceClock.bind(service);gate.service=&service;
    assert(service.apply(f.s.config));
    // open() correctly freezes a single-peer runtime when its only peer is
    // durably revoked; it cannot authorize RF startup. Cleanup remains an
    // idempotent tombstone operation while the local identity is still bound.
    assert(!service.start(false,0)&&!service.receiving()&&!service.faulted());
    const auto writes=f.lm.writes;
    assert(service.revokePrivatePeer(f.peer.binding.node)&&!service.faulted()&&f.lm.writes==writes);
    assert(!runtime.ready(f.local.binding.node)&&replay.revoked(f.peer.binding.node));
    assert(rebootIdentity.close()&&!replay.revoked(f.peer.binding.node));
    group("fresh-object tombstone cleanup requires bound active identity and never rewrites or restores trust");}
  for(size_t length:{size_t(166),size_t(167),size_t(200),size_t(232)}) {
    Fixture f(true);
    const std::vector<uint8_t> payload(length,'x');
    uint32_t id=0;
    assert(!f.s.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,payload.data(),payload.size(),
      0,false,100,901,id));
    assert(id==901&&f.ledger.sent(id)&&f.entropy.calls==1);
    assert(f.signing.calls==(length<=166?1u:0u)&&f.s.receiving()&&!f.s.faulted());
    f.poll(1000);
    assert(f.radio.frames.size()==1);
    const auto &frame=f.radio.frames.front();
    bench::RouteMetadata metadata;
    assert(f.s.codec.decode(frame.data(),frame.size(),metadata));
    assert(metadata.portnum==1&&metadata.payloadLength==length&&!memcmp(metadata.payload,payload.data(),length));
    assert(metadata.hasBitfield&&!metadata.bitfield&&metadata.signatureLength==(length<=166?64:0));
    assert(frame.size()==(length<=166?length+89:length+23));
  }
  group("stock text size boundary signs 166 bytes and sends 167/200/232 unsigned with one durable outgoing ID");
  {
    Fixture f(true);
    const uint8_t text[]={'a','b'};
    f.signing.fail=true;
    uint32_t id=1;
    assert(f.s.submitLocalInput(1,text,sizeof(text),0,false,100,902,id)<0&&!id);
    assert(f.ledger.sent(902)&&f.signing.calls==1&&f.s.endpointFree()==4&&!f.radio.sends);
    assert(f.s.receiving()&&!f.s.faulted());
    const std::vector<uint8_t> tooLarge(233,'x');
    assert(f.s.submitLocalInput(1,tooLarge.data(),tooLarge.size(),0,false,100,903,id)==-EMSGSIZE&&!id);
    assert(!f.ledger.sent(903)&&!f.radio.sends);
    group("fitting-frame signing failure never falls back; unsigned oversize rejects before reserving an ID");
  }
}
