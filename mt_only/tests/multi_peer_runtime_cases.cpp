// SPDX-License-Identifier: GPL-3.0-only
// Real PSA/session/journal/Endpoint tests with synthetic keys. The always-ready
// storage gate is a test boundary, not proof of the native RF storage barrier.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"

namespace {
struct Quiescent:LedgerStorageGate {bool quiescent()const override{return true;}};
struct Four {
  Key local{11},peers[4]{Key{97},Key{61},Key{149},Key{193}},stranger{227};
  Memory im,lm;MonocypherIdentityDeriver deriver;IdentityJournal identity;PkiLedger ledger;
  Quiescent gate;Entropy entropy;SignEntropy signing;Backend backend;
  mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,lm,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,lm,gate};
  MultiPeerRuntime runtime{crypto,backend,identity,nonce,replay,&signing};
  PkiPeerSet set;mt::Endpoint endpoint;bench::MtCodec codec;bench::MtNodeRecord record;
  explicit Four(unsigned count=4,bool open=true) {
    assert(count>=1&&count<=4);
    assert(!identity.load(im,deriver));IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true));assert(!ledger.load(lm));
    assert(!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    PkiPeerPin pins[4]{};
    for(unsigned i=0;i<count;++i) {
      pins[i].node=peers[i].binding.node;memcpy(pins[i].publicKey,peers[i].binding.publicKey,32);
      assert(!ledger.addTrustedPeer(lm,pins[i].node,pins[i].publicKey,true));
    }
    assert(set.assign(local.binding,pins,count));
    record.setPublic();assert(codec.setKey(record.bytes[4],record.bytes+bench::MtNodeRecord::Key,16));
    if(open)assert(runtime.open(local.binding.node,set));
  }
  int send(unsigned peer,uint32_t id,uint8_t owner=0,bool reliable=false,const uint8_t *key=nullptr,size_t keySize=0) {
    const auto plain=data();return runtime.submit(endpoint,owner,peers[peer].binding.node,id,plain.data(),plain.size(),0,reliable,key,keySize,true,100);
  }
  Bytes transmit(uint32_t now=1000) {
    auto *job=endpoint.next(now);assert(job);Bytes bytes(job->frame.bytes,job->frame.bytes+job->frame.len);
    endpoint.started(*job,0,now,1,0);endpoint.complete(*job,now+1,0);return bytes;
  }
  Bytes incoming(unsigned peer,uint32_t id,const Key *encryptor=nullptr,const Bytes *payload=nullptr) {
    const auto text=data("synthetic peer text");const auto &plain=payload?*payload:text;uint8_t encrypted[239]{};size_t size=0;
    assert(crypto.encrypt(encryptor?encryptor->scalar:peers[peer].scalar,local.binding.publicKey,
      peers[peer].binding.node,id,0x19374628,plain.data(),plain.size(),encrypted,sizeof(encrypted),size));
    Bytes frame(16+size);put(frame.data(),local.binding.node);put(frame.data()+4,peers[peer].binding.node);
    put(frame.data()+8,id);frame[15]=uint8_t(peers[peer].binding.node);memcpy(frame.data()+16,encrypted,size);return frame;
  }
  void receive(const Bytes &frame,uint32_t now=1500) {runtime.receive(endpoint,codec,frame.data(),frame.size(),now,701,false);}
  bool event(uint8_t owner,const Bytes &frame,PrivateEvent &out) {return runtime.event(owner,frame.data(),frame.size(),out);}
  Bytes ack(unsigned signer,unsigned source,uint32_t request) {
    uint8_t routing[12]{},raw[2]={24,0};size_t size=0;mt::experimental::PkiAck proof(crypto);
    assert(proof.attach(peers[signer].scalar,local.binding.publicKey,
      {true,peers[source].binding.node,local.binding.node,request},raw,2,routing,sizeof(routing),size));
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_ROUTING_APP;d.request_id=request;d.has_bitfield=true;
    d.payload.size=size;memcpy(d.payload.bytes,routing,size);uint8_t bytes[227]{},frame[255]{};
    auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Data_fields,&d));
    size_t length=0;assert(codec.encodeZeroHopData(peers[source].binding.node,local.binding.node,
      request+1000,bytes,out.bytes_written,frame,sizeof(frame),length));return {frame,frame+length};
  }
  void proof(const Bytes &frame,uint32_t now=1500) {
    bench::RouteMetadata metadata;assert(codec.decode(frame.data(),frame.size(),metadata));
    runtime.proof(endpoint,frame.data(),frame.size(),metadata,now);
  }
};
// The facade's public peer lookup must work for every active pin, not only
// peer(), which remains the first-active legacy accessor. These frames are
// produced by the actual signer with synthetic keys and no NodeInfo bootstrap.
Bytes signedPublic(Four &f,unsigned peer,uint32_t id){
  Memory memory;MonocypherIdentityDeriver deriver;IdentityJournal identity;
  const auto &sender=f.peers[peer];assert(!identity.load(memory,deriver));
  IdentityCandidate candidate(sender.binding,sender.scalar);assert(!identity.store(memory,deriver,candidate,true));
  IdentityBinding binding;KeyRef ref;assert(identity.binding(binding,ref));SignEntropy entropy;SignedData signedData;
  const auto plain=data("multi-peer signed Public without NodeInfo");meshtastic_Data d{};
  auto in=pb_istream_from_buffer(plain.data(),plain.size());assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left);
  assert(PkiSigner::sign(identity,ref,binding,entropy,id,0xffffffffu,d,signedData)==SigningResult::Ok);
  uint8_t wire[255]{};size_t size=0;assert(f.codec.encodeSignedApplication(sender.binding.node,id,signedData.bytes,signedData.size,wire,sizeof(wire),size));
  return {wire,wire+size};
}
struct PublicView {
  Four &four;Radio radio;mt::Service service;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion c,bc;
  explicit PublicView(Four &f):four(f),service(radio,true,f.local.binding.node,127,true,false,&f.runtime),
    c(service,usb,127,workspace,0),bc(service,ble,127,workspace,1){
    mt::Config config;config.bytes[4]=1;auto record=config.node();record.setPublic();config.node(record);
    assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=3;
    input(c,r,1);input(bc,r,1);usb.out.clear();ble.out.clear();usb.records.clear();ble.records.clear();
  }
  void receive(const Bytes &wire,uint32_t now){
    service.rx(wire.data(),wire.size(),3,now,711);
    c.receive(wire.data(),wire.size(),-71,3,now,1792660000u+now/1000,true,12,true,mt::Companion::WorkspaceProof{true});
    bc.receive(wire.data(),wire.size(),-71,3,now,1792660000u+now/1000,true,12,true,mt::Companion::WorkspaceProof{true});
  }
  unsigned delivered(const Host &host,unsigned peer,uint32_t id)const{
    unsigned count=0;for(const auto &r:host.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.id==id){
      assert(r.packet.from==four.peers[peer].binding.node&&r.packet.to==0xffffffffu&&!r.packet.pki_encrypted&&
        r.packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&!r.packet.decoded.request_id);
      constexpr char expected[]="multi-peer signed Public without NodeInfo";
      assert(r.packet.decoded.payload.size==sizeof(expected)-1&&!memcmp(r.packet.decoded.payload.bytes,expected,sizeof(expected)-1));++count;}
    return count;
  }
};
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
}

// Regression: ignoring the first/legacy active peer must not block private
// sends to a different active peer, while the ignored destination stays
// refused. Baseline is expected to fail until the paired Service changes
// (per-destination ignore) land.
static void RuntimeIgnorePeerCase() {
  Four four;
  PublicView view{four};
  auto &nodes=view.service.nodes;
  const uint32_t self=four.local.binding.node;
  const uint32_t ignored=four.peers[0].binding.node; // first/legacy active peer
  const uint32_t other=four.peers[1].binding.node;   // a different active peer

  // Ignore the first active peer through the exact local-admin API.
  assert(nodes.edit(ignored,self,mt::NodeCache::Edit::Ignore)==1);
  // Per-destination state: only the ignored peer is refused.
  assert(nodes.ignored(ignored));
  assert(!nodes.ignored(other));

  const auto plain=data();
  // The ignored destination stays refused.
  assert(view.service.submitPrivate(0,ignored,1,plain.data(),plain.size(),0,false,nullptr,0,true,100)==-EPERM);
  // A different active peer is not blocked by the ignore on the first peer.
  assert(view.service.submitPrivate(0,other,2,plain.data(),plain.size(),0,false,nullptr,0,true,100)==0);
}

int main() {
  {Four f(3);constexpr uint8_t query[]={8,3,24,1,72,2};bench::Frame out;
    uint8_t key[16]{};key[0]=0x51;mt::PrimaryChannel::Selection selection;
    assert(mt::PrimaryChannel::normalize(key,sizeof(key),selection)&&f.codec.setKey(selection.hash,selection.bytes,selection.length));
    mt::PrimaryChannel::clear(selection);const auto entropy=f.entropy.calls;
    assert(f.runtime.encodeChannelControl(f.codec,f.stranger.binding.node,1600,query,sizeof(query),out));
    bench::RouteMetadata m;assert(f.codec.decode(out.bytes,out.len,m)&&m.portnum==3&&m.wantResponse&&
      bench::le32(out.bytes)==f.stranger.binding.node&&f.ledger.sent(1600)&&f.entropy.calls==entropy+1);
    assert(f.runtime.revoke(f.peers[0].binding.node)&&f.runtime.ready(f.local.binding.node));
    assert(f.runtime.encodeChannelControl(f.codec,f.stranger.binding.node,1601,query,sizeof(query),out)&&f.ledger.sent(1601));
    assert(!f.send(1,1602));const auto after=f.entropy.calls;const auto before=out;
    assert(!f.runtime.encodeChannelControl(f.codec,f.stranger.binding.node,1602,query,sizeof(query),out)&&
      f.entropy.calls==after&&out.len==before.len&&!memcmp(out.bytes,before.bytes,sizeof(out.bytes)));
    pass("custom channel control selects active local authority for an unpinned public recipient, survives one revoke and shares private outgoing-ID domain");}
  {Four f;constexpr uint8_t query[]={8,3,24,1,72,2};bench::Frame out;memset(out.bytes,0x42,sizeof(out.bytes));out.len=123;
    f.lm.onWrite=[&]{assert(f.identity.close());};
    assert(!f.runtime.encodeChannelControl(f.codec,f.stranger.binding.node,1700,query,sizeof(query),out)&&
      !f.runtime.ready(f.local.binding.node)&&out.len==123);
    for(const auto byte:out.bytes)assert(byte==0x42);
    pass("facade readiness loss during reservation freezes without publishing candidate output");}
  RuntimeIgnorePeerCase();pass("ignored-first-peer-does-not-disable-other-private-destinations");
{
  Four f(2);
  auto forged=f.incoming(0,0x4e20,&f.stranger);f.runtime.receive(f.endpoint,f.codec,forged.data(),forged.size(),1499,701,true);
  uint32_t observed=0;assert(!f.runtime.takeAuthenticatedReceive(observed)&&observed==0);
  auto a0=f.incoming(0,0x4e21);a0[12]|=8;
  auto a1=f.incoming(1,0x4e21);a1[12]|=8;
  f.runtime.receive(f.endpoint,f.codec,a0.data(),a0.size(),1500,701,true);
  assert(f.runtime.takeAuthenticatedReceive(observed)&&observed==f.peers[0].binding.node);
  f.runtime.receive(f.endpoint,f.codec,a1.data(),a1.size(),1501,701,true);
  assert(f.runtime.takeAuthenticatedReceive(observed)&&observed==f.peers[1].binding.node);
  auto r0=f.transmit(2000);assert(bench::le32(r0.data())==f.peers[0].binding.node&&bench::le32(r0.data()+4)==f.local.binding.node);
  auto r1=f.transmit(2200);assert(bench::le32(r1.data())==f.peers[1].binding.node&&bench::le32(r1.data()+4)==f.local.binding.node);
  f.runtime.receive(f.endpoint,f.codec,a0.data(),a0.size(),2500,701,true);
  f.runtime.receive(f.endpoint,f.codec,a1.data(),a1.size(),2501,701,true);
  assert(!f.runtime.takeAuthenticatedReceive(observed)&&observed==0);
  auto d0=f.transmit(3000);assert(bench::le32(d0.data())==f.peers[0].binding.node&&bench::le32(d0.data()+4)==f.local.binding.node);
  auto d1=f.transmit(3200);assert(bench::le32(d1.data())==f.peers[1].binding.node&&bench::le32(d1.data()+4)==f.local.binding.node);
  pass("authenticated-observer-rejects-forgery-and-multiplexes-two-peer-fresh-receives-once");
}
  {
    // Observes suppression at the revoked/inactive-peer gate only; not proof of internal duplicate-ACK-cache clearing.
    Four f(2);
    auto frame=f.incoming(0,f.record.bytes[4]);
    frame[12]|=8;
    f.runtime.receive(f.endpoint,f.codec,frame.data(),frame.size(),1500,701,true);
    auto ack=f.transmit(2000);
    assert(bench::le32(ack.data())==f.peers[0].binding.node);
    assert(bench::le32(ack.data()+4)==f.local.binding.node);
    assert(f.runtime.revoke(f.peers[0].binding.node));
    assert(f.runtime.ready(f.local.binding.node));
    assert(f.runtime.peer()==f.peers[1].binding.node);
    f.runtime.receive(f.endpoint,f.codec,frame.data(),frame.size(),3000,701,true);
    assert(f.endpoint.next(3000)==nullptr);
    pass("revoked-inactive-peer-gate-suppresses-replayed-ack");
  }
  for(unsigned count:{1u,4u}) {
    Four f(count);uint8_t key[32]{};const auto writes=f.lm.writes;
    assert(f.runtime.ready(f.local.binding.node)&&!f.runtime.ready(f.stranger.binding.node));
    assert(f.runtime.publicIdentity(key)&&!memcmp(key,f.local.binding.publicKey,32));
    assert(f.runtime.peer()==f.peers[0].binding.node&&!f.runtime.open(f.local.binding.node,f.set));
    assert(f.lm.writes==writes&&!f.entropy.calls);
    pass(count==1?"one-peer-compatible-open-and-original-public-identity":"four-peer-open-does-not-write-or-reserve-nonces");
  }
  {Four f(4,false);PkiPeerPin pins[4]{};
    for(unsigned i=0;i<4;++i){const Key &key=i==3?f.stranger:f.peers[i];pins[i].node=key.binding.node;memcpy(pins[i].publicKey,key.binding.publicKey,32);}
    PkiPeerSet absent;assert(absent.assign(f.local.binding,pins,4));const auto writes=f.lm.writes;
    assert(!f.runtime.open(f.local.binding.node,absent)&&!f.runtime.ready(f.local.binding.node));
    assert(!f.runtime.open(f.local.binding.node,f.set)&&f.lm.writes==writes&&!f.entropy.calls);
    uint8_t key[32]{};assert(!f.runtime.publicIdentity(key));
    pass("partial-open-missing-authoritative-pin-freezes-all-and-cannot-reopen");}
  {Four f(1,false);PkiPeerSet empty;
    assert(f.runtime.open(f.local.binding.node,empty)&&f.runtime.dormant(f.local.binding.node)&&!f.runtime.ready(f.local.binding.node));
    assert(!f.runtime.open(f.local.binding.node,f.set));
    pass("verified-empty-open-is-dormant-and-never-selects-default-peer");}
  {Four f;const auto plain=data();const auto writes=f.lm.writes,encrypts=f.backend.encrypts,calls=f.entropy.calls;
    assert(f.runtime.submit(f.endpoint,0,f.stranger.binding.node,70,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
    assert(f.send(0,70,0,false,f.peers[1].binding.publicKey,32)<0);
    assert(f.lm.writes==writes&&f.backend.encrypts==encrypts&&f.entropy.calls==calls&&f.endpoint.free()==4);
    pass("unknown-destination-and-wrong-explicit-key-never-consume-nonce-or-encrypt");}
  {Four f;
    for(unsigned i=0;i<4;++i)assert(!f.send(i,100+i));
    assert(f.endpoint.free()==0);const auto calls=f.entropy.calls,encrypts=f.backend.encrypts;
    assert(f.send(0,104)<0&&f.entropy.calls==calls&&f.backend.encrypts==encrypts);
    for(unsigned i=0;i<4;++i){auto wire=f.transmit();assert(bench::le32(wire.data())==f.peers[i].binding.node);
      uint8_t plain[227]{};size_t size=0;assert(f.crypto.decrypt(f.peers[i].scalar,f.local.binding.publicKey,
        f.local.binding.node,100+i,wire.data()+16,wire.size()-16,plain,sizeof(plain),size));
      const auto expected=data();assert(size==expected.size()&&!memcmp(plain,expected.data(),size));}
    f.runtime.sweep(f.endpoint);assert(f.endpoint.free()==4);
    pass("four-destinations-use-exact-peer-keys-and-shared-four-job-capacity");}
  {Four f;assert(!f.send(0,110));const auto writes=f.lm.writes,encrypts=f.backend.encrypts,calls=f.entropy.calls;
    assert(f.send(1,110)<0&&f.lm.writes==writes&&f.backend.encrypts==encrypts&&f.entropy.calls==calls);
    assert(!f.send(0,110,1)&&f.lm.writes==writes&&f.backend.encrypts==encrypts&&f.entropy.calls==calls);
    assert(f.endpoint.pendingPrivate(110,f.peers[0].binding.node)&&!f.endpoint.pendingPrivate(110,f.peers[1].binding.node));
    pass("cross-peer-ID-retry-refused-but-same-peer-owner-merge-reuses-ciphertext");}
  {Four f;PrivateEvent event;
    for(unsigned i=0;i<4;++i){auto wire=f.incoming(i,500);const auto writes=f.lm.writes;f.receive(wire,1500+i);
      assert(f.lm.writes==writes+1&&f.event(0,wire,event)&&event.from==f.peers[i].binding.node);
      assert(!memcmp(event.publicKey,f.peers[i].binding.publicKey,32)&&f.event(1,wire,event)&&!f.event(1,wire,event));
      f.receive(wire,1600+i);assert(f.lm.writes==writes+1&&!f.event(0,wire,event));}
    pass("same-incoming-ID-is-per-peer-durable-replay-and-fanout-once-per-owner");}
  {Four f;PrivateEvent event;auto old=f.incoming(0,600);f.receive(old);assert(f.event(0,old,event));
    auto next=f.incoming(1,601);f.receive(next,1600);assert(!f.event(1,old,event));
    assert(f.event(0,next,event)&&event.from==f.peers[1].binding.node&&f.event(1,next,event));
    f.runtime.clearEvent();assert(!f.event(0,next,event));
    pass("every-new-RX-clears-all-stale-child-events-and-explicit-clear-is-global");}
  {Four f;PrivateEvent event;auto old=f.incoming(0,610);f.receive(old);assert(f.event(0,old,event));
    auto unknown=f.incoming(1,611);put(unknown.data()+4,f.stranger.binding.node);
    const auto writes=f.lm.writes,decrypts=f.backend.decrypts,calls=f.entropy.calls;f.receive(unknown,1600);
    assert(!f.event(1,old,event)&&!f.event(0,unknown,event)&&f.lm.writes==writes&&f.backend.decrypts==decrypts&&f.entropy.calls==calls);
    auto wrong=f.incoming(0,612,&f.peers[1]);const auto badWrites=f.lm.writes;f.receive(wrong,1700);
    assert(!f.event(0,wrong,event)&&f.lm.writes==badWrites&&f.runtime.ready(f.local.binding.node));
    pass("unknown-source-clears-stale-event-without-decrypt-and-cross-peer-key-cannot-authenticate");}
  {Four f;auto wire=f.incoming(0,613);put(wire.data(),f.peers[1].binding.node);
    const auto writes=f.lm.writes,decrypts=f.backend.decrypts;f.receive(wire);PrivateEvent event;
    assert(!f.event(0,wire,event)&&f.lm.writes==writes&&f.backend.decrypts==decrypts);
    f.runtime.receive(f.endpoint,f.codec,nullptr,99,1600,701,false);uint8_t shortFrame[7]{};
    f.runtime.receive(f.endpoint,f.codec,shortFrame,sizeof(shortFrame),1700,701,false);
    assert(f.runtime.ready(f.local.binding.node)&&f.lm.writes==writes&&f.backend.decrypts==decrypts);
    pass("wrong-private-destination-null-and-short-frames-never-enter-a-session");}
  {Four f;assert(!f.send(0,120,0,true));f.transmit();
    f.proof(f.ack(1,1,120));assert(f.endpoint.pendingPrivate(120,f.peers[0].binding.node,true));
    f.proof(f.ack(1,0,120),1600);assert(f.endpoint.pendingPrivate(120,f.peers[0].binding.node,true));
    f.proof(f.ack(0,0,120),1700);mt::Endpoint::Result result;
    assert(f.endpoint.take(0,result)&&result.kind==mt::Endpoint::Kind::Authenticated&&result.peer==f.peers[0].binding.node&&result.id==120);
    assert(!f.endpoint.take(0,result));pass("ACK-proof-dispatch-requires-actual-source-peer-key-and-pending-destination");}
  {Four f;auto wire=f.incoming(0,620);f.receive(wire);PrivateEvent event;assert(f.event(0,wire,event));
    f.proof(f.ack(1,1,999));assert(!f.event(1,wire,event));
    pass("new-proof-RX-also-clears-prior-child-plaintext-event");}
  {Four f;meshtastic_Data query{};query.portnum=meshtastic_PortNum_TELEMETRY_APP;
    query.want_response=true;query.has_bitfield=true;query.bitfield=2;uint8_t bytes[227]{};
    auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Data_fields,&query));
    for(uint8_t owner:{0,1})assert(!f.runtime.submit(f.endpoint,owner,f.peers[0].binding.node,125,bytes,out.bytes_written,0,false,nullptr,0,true,100));
    f.transmit();uint8_t response[40]{};size_t size=0;assert(mt::TelemetryQuery::uptimeResponse(1234,125,response,sizeof(response),size));
    const Bytes payload(response,response+size);auto wrong=f.incoming(1,630,nullptr,&payload);f.receive(wrong);PrivateEvent event;
    assert(!f.event(0,wrong,event)&&f.endpoint.pendingPrivate(125,f.peers[0].binding.node));
    auto good=f.incoming(0,631,nullptr,&payload);f.receive(good,1600);
    assert(f.event(0,good,event)&&event.from==f.peers[0].binding.node&&f.event(1,good,event));
    for(uint8_t owner:{0,1}){mt::Endpoint::Result result;assert(f.endpoint.take(owner,result)&&result.kind==mt::Endpoint::Kind::PrivateResponse&&result.peer==f.peers[0].binding.node&&result.id==125);}
    pass("private-DeviceMetrics-reply-request-correlation-cannot-cross-selected-peers");}
  {Four f;assert(!f.send(1,130));const auto im=f.im.values;assert(f.runtime.revoke(f.peers[0].binding.node));
    assert(f.runtime.ready(f.local.binding.node)&&f.runtime.peer()==f.peers[1].binding.node&&f.ledger.revoked(f.peers[0].binding.node));
    const auto writes=f.lm.writes;assert(f.runtime.revoke(f.peers[0].binding.node)&&f.lm.writes==writes);
    f.runtime.sweep(f.endpoint);assert(f.endpoint.pendingPrivate(130,f.peers[1].binding.node));
    uint8_t key[32]{};assert(f.runtime.publicIdentity(key)&&!memcmp(key,f.local.binding.publicKey,32)&&f.im.values==im);
    assert(!f.runtime.acceptsPeerKey(f.peers[0].binding.node,f.peers[0].binding.publicKey,32));key[0]^=1;
    assert(!f.runtime.acceptsPeerKey(f.peers[0].binding.node,key,32)&&f.runtime.acceptsPeerKey(f.stranger.binding.node,key,32));
    assert(f.send(0,131)<0);bench::Frame frame;assert(f.runtime.bootstrap(f.codec,f.record,81,0xffffffffu,132,false,0,frame));
    // Facade-only queue observation: actual Service revocation deliberately
    // stops/disarms and cancels ALL jobs before any later radio work.
    pass("successful-peer-revoke-retains-siblings-and-rejects-all-keys-for-hard-tombstone");}
  {Four f;assert(!f.send(1,140));f.lm.fault=1;assert(!f.runtime.revoke(f.peers[0].binding.node));
    assert(!f.runtime.ready(f.local.binding.node));f.runtime.sweep(f.endpoint);mt::Endpoint::Result result;
    assert(f.endpoint.take(0,result)&&result.kind==mt::Endpoint::Kind::Failed&&!f.endpoint.take(0,result));
    f.runtime.sweep(f.endpoint);assert(!f.endpoint.take(0,result));
    pass("failed-revocation-freezes-all-and-global-sweep-cancels-private-once");}
  {Four f;assert(!f.send(0,150));assert(!f.ledger.revokeTrustedPeer(f.lm,f.peers[2].binding.node,f.peers[2].binding.publicKey,true));
    assert(!f.runtime.ready(f.local.binding.node));f.runtime.sweep(f.endpoint);assert(f.endpoint.free()==4);
    const auto writes=f.lm.writes,encrypts=f.backend.encrypts;assert(f.send(1,151)<0&&f.lm.writes==writes&&f.backend.encrypts==encrypts);
    pass("unexpected-single-child-authority-failure-removes-global-readiness-with-no-fallback");}
  {Four f;f.backend.failure=true;assert(f.send(3,160)<0&&!f.runtime.ready(f.local.binding.node));
    uint8_t key[32]{};assert(!f.runtime.publicIdentity(key));pass("one-child-crypto-failure-freezes-global-identity-capability");}
  {Four f;bench::Frame frame;const auto identity=f.im.values;
    assert(f.runtime.bootstrap(f.codec,f.record,81,0xffffffffu,170,false,0,frame)&&f.ledger.sent(170));
    const auto calls=f.entropy.calls,encrypts=f.backend.encrypts;assert(f.send(3,170)<0&&f.entropy.calls==calls&&f.backend.encrypts==encrypts);
    const auto plain=data();assert(f.runtime.broadcast(f.codec,171,plain.data(),plain.size(),frame)&&f.ledger.sent(171)&&f.im.values==identity);
    pass("signed-public-and-every-private-peer-share-original-identity-and-global-nonce-domain");}
  {Four f;auto wire=f.incoming(2,700);f.receive(wire);f.runtime.freeze();
    IdentityJournal identity;PkiLedger ledger;assert(!identity.load(f.im,f.deriver)&&!ledger.load(f.lm));
    LedgerNonceAuthority nonce{identity,ledger,f.lm,f.entropy,f.gate};LedgerReplayAuthority replay{identity,ledger,f.lm,f.gate};
    MultiPeerRuntime restarted{f.crypto,f.backend,identity,nonce,replay,&f.signing};assert(restarted.open(f.local.binding.node,f.set));
    const auto writes=f.lm.writes;restarted.receive(f.endpoint,f.codec,wire.data(),wire.size(),2000,701,false);PrivateEvent event;
    assert(!restarted.event(0,wire.data(),wire.size(),event)&&f.lm.writes==writes);
    pass("fresh-four-peer-runtime-reuses-durable-per-peer-replay-history-after-reload");}
  {Four f(1);
    for(unsigned i=0;i<40;++i){
      const uint32_t id=2000+i;assert(!f.send(0,id));auto wire=f.transmit(1000+i*10);
      uint8_t plain[227]{};size_t size=0;
      assert(f.crypto.decrypt(f.peers[0].scalar,f.local.binding.publicKey,f.local.binding.node,id,
        wire.data()+16,wire.size()-16,plain,sizeof(plain),size));
      const auto expected=data();assert(size==expected.size()&&!memcmp(plain,expected.data(),size));
      f.runtime.sweep(f.endpoint);mt::Endpoint::Result result;
      assert(f.endpoint.take(0,result)&&result.id==id&&result.kind==mt::Endpoint::Kind::Local);
      assert(!f.endpoint.take(0,result)&&f.endpoint.free()==4&&f.runtime.ready(f.local.binding.node));
    }
    assert(f.entropy.calls==40&&f.backend.encrypts==40);PkiLedger::Status status;
    assert(f.ledger.status(status)&&status.sent==40);const auto calls=f.entropy.calls,writes=f.lm.writes;
    assert(f.send(0,2000)<0&&f.entropy.calls==calls&&f.lm.writes==writes&&f.runtime.ready(f.local.binding.node));
    pass("forty-completed-same-peer-real-crypto-sends-cross-old32-RAM-limit-with-durable-ID-reuse-denied");}
  {Four f;const auto identity=f.im.values,ledger=f.lm.values;const auto iw=f.im.writes,lw=f.lm.writes;
    for(unsigned i=0;i<4;++i){uint8_t key[32]{};assert(f.runtime.peerPublicIdentity(f.peers[i].binding.node,key)&&!memcmp(key,f.peers[i].binding.publicKey,32));}
    uint8_t sentinel[32];memset(sentinel,0x5a,sizeof(sentinel));assert(!f.runtime.peerPublicIdentity(f.stranger.binding.node,sentinel));
    for(auto v:sentinel){assert(v==0x5a);}assert(!f.runtime.peerPublicIdentity(f.peers[0].binding.node,nullptr));
    assert(f.runtime.peer()==f.peers[0].binding.node&&f.im.values==identity&&f.lm.values==ledger&&f.im.writes==iw&&f.lm.writes==lw&&!f.entropy.calls);
    pass("all-four-public-peer-identities-are-exact-active-pins-and-unknown-lookup-preserves-output");}
  {Four f;PublicView view(f);const auto identity=f.im.values,ledger=f.lm.values;
    const auto iw=f.im.writes,lw=f.lm.writes,calls=f.entropy.calls,encrypts=f.backend.encrypts,decrypts=f.backend.decrypts;
    assert(!view.service.signature.keyCount(1500)&&!view.service.nodes.count());
    // Keep the actual verifier clock monotonic across original/duplicate pairs.
    // Backward unsigned time intentionally expires future-seen cache entries.
    for(unsigned i=0;i<4;++i){const auto frame=signedPublic(f,i,8100+i);view.receive(frame,1500+2*i);
      assert(view.delivered(view.usb,i,8100+i)==1&&view.delivered(view.ble,i,8100+i)==1);
      view.receive(frame,1501+2*i);assert(view.delivered(view.usb,i,8100+i)==1&&view.delivered(view.ble,i,8100+i)==1);
      assert(view.service.signature.keyCount(1501+2*i)==i+1);}
    assert(view.service.signature.keyCount(1700)==4&&view.service.nodes.count()==4&&view.service.nodes.dirty()&&!view.radio.sends);
    bool seenSigner[4]{};
    for(unsigned i=0;i<view.service.nodes.count();++i){
      const auto *entry=view.service.nodes.get(i);assert(entry&&entry->placeholder()&&entry->verifiedSigner());
      assert(entry->bytes[mt::NodeCache::Entry::LocalFlags]==
        (mt::NodeCache::Entry::Placeholder|mt::NodeCache::Entry::VerifiedSigner));
      for(unsigned byte=4;byte<=mt::NodeCache::Entry::Flags;++byte)assert(entry->bytes[byte]==0);
      assert(entry->bytes[mt::NodeCache::Entry::KeyLength]==0&&entry->model()==0&&entry->role()==0);
      bool knownSigner=false;for(unsigned peer=0;peer<4;++peer)if(entry->id()==f.peers[peer].binding.node){
        assert(!seenSigner[peer]);seenSigner[peer]=true;knownSigner=true;}
      assert(knownSigner);
    }
    for(bool seen:seenSigner)assert(seen);
    assert(f.im.values==identity&&f.lm.values==ledger&&f.im.writes==iw&&f.lm.writes==lw&&f.entropy.calls==calls&&f.backend.encrypts==encrypts&&f.backend.decrypts==decrypts);
    pass("all-four-active-pins-verify-Public-fanout-once-with-signer-placeholders-but-no-contact-trust-or-ledger-writes");}
  {Four f;PublicView view(f);const auto revoked=signedPublic(f,0,8200),active=signedPublic(f,3,8201);
    assert(f.runtime.revoke(f.peers[0].binding.node));const auto identity=f.im.values,ledger=f.lm.values;
    const auto iw=f.im.writes,lw=f.lm.writes;uint8_t key[32];memset(key,0x5a,sizeof(key));
    assert(!f.runtime.peerPublicIdentity(f.peers[0].binding.node,key));for(auto v:key){assert(v==0x5a);}
    assert(f.runtime.peerPublicIdentity(f.peers[3].binding.node,key)&&!memcmp(key,f.peers[3].binding.publicKey,32));
    bench::RouteMetadata m;assert(view.service.codec.decode(revoked.data(),revoked.size(),m));
    view.service.applyPublicSignature(revoked.data(),revoked.size(),m,1500);assert(m.signatureRejected&&!m.signatureVerified);
    view.receive(revoked,1500);assert(view.usb.out.empty()&&view.ble.out.empty());view.receive(active,1600);
    assert(view.delivered(view.usb,3,8201)==1&&view.delivered(view.ble,3,8201)==1&&view.service.signature.keyCount(1600)==1);
    assert(f.im.values==identity&&f.lm.values==ledger&&f.im.writes==iw&&f.lm.writes==lw&&!f.entropy.calls&&!view.radio.sends);
    // Facade-level revocation leaves other active pins usable; Service also
    // restores RX/arm after its selective, quiescent revocation barrier.
    pass("revoked-facade-pin-cannot-refresh-public-authority-while-another-active-pin-remains-exact");}
  {for(bool frozen:{false,true}){Four f;PublicView view(f);const auto frame=signedPublic(f,3,8300);
      if(frozen)f.runtime.freeze();else assert(!f.ledger.revokeTrustedPeer(f.lm,f.peers[1].binding.node,f.peers[1].binding.publicKey,true));
      const auto identity=f.im.values,ledger=f.lm.values;const auto iw=f.im.writes,lw=f.lm.writes;
      for(unsigned i=0;i<4;++i){uint8_t key[32];memset(key,0x5a,sizeof(key));assert(!f.runtime.peerPublicIdentity(f.peers[i].binding.node,key));for(auto v:key){assert(v==0x5a);}}
      bench::RouteMetadata m;assert(view.service.codec.decode(frame.data(),frame.size(),m));
      view.service.applyPublicSignature(frame.data(),frame.size(),m,1500);assert(m.signatureRejected&&!m.signatureVerified&&!m.signatureQuarantined);
      view.receive(frame,1500);assert(view.usb.out.empty()&&view.ble.out.empty()&&!view.service.signature.keyCount(1500)&&!view.radio.sends);
      assert(f.im.values==identity&&f.lm.values==ledger&&f.im.writes==iw&&f.lm.writes==lw&&!f.entropy.calls);}
    pass("one-child-authority-loss-or-global-freeze-blocks-every-Public-peer-pin-without-fallback-or-storage-write");}
  {Four f;const uint32_t target=f.peers[2].binding.node;const auto plain=data("native UI private");
    const auto writes=f.lm.writes,calls=f.entropy.calls,encrypts=f.backend.encrypts;
    assert(f.runtime.peer()==f.peers[0].binding.node&&f.runtime.submit(f.endpoint,mt::Endpoint::NoClientOwner,
      target,8400,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
    assert(f.lm.writes==writes&&f.entropy.calls==calls&&f.backend.encrypts==encrypts&&!f.ledger.sent(8400));
    assert(f.runtime.submit(f.endpoint,0xfd,target,8407,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
    assert(f.lm.writes==writes&&f.entropy.calls==calls&&f.backend.encrypts==encrypts&&!f.ledger.sent(8407));
    assert(f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,target,8401,plain.data(),plain.size(),0,false,
      nullptr,0,true,100)==0);
    const auto retryWrites=f.lm.writes,retryCalls=f.entropy.calls,retryEncrypts=f.backend.encrypts;
    assert(f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,target,8401,plain.data(),plain.size(),0,false,
      nullptr,0,true,101)==0);
    auto changed=plain;changed.back()^=1;
    assert(f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,target,8401,changed.data(),changed.size(),0,false,
      nullptr,0,true,102)<0);
    assert(f.lm.writes==retryWrites&&f.entropy.calls==retryCalls&&f.backend.encrypts==retryEncrypts);
    auto *job=f.endpoint.next(1000);assert(job&&bench::le32(job->frame.bytes)==target&&bench::le32(job->frame.bytes+8)==8401);
    Bytes wire(job->frame.bytes,job->frame.bytes+job->frame.len);uint8_t decoded[227]{};size_t decodedSize=0;
    assert(f.crypto.decrypt(f.peers[2].scalar,f.local.binding.publicKey,f.local.binding.node,8401,
      wire.data()+16,wire.size()-16,decoded,sizeof(decoded),decodedSize));
    const auto firstPlain=data("native UI private");assert(decodedSize==firstPlain.size()&&!memcmp(decoded,firstPlain.data(),decodedSize));
    assert(!f.crypto.decrypt(f.peers[0].scalar,f.local.binding.publicKey,f.local.binding.node,8401,
      wire.data()+16,wire.size()-16,decoded,sizeof(decoded),decodedSize));
    f.endpoint.started(*job,0,1000,1,0);f.endpoint.complete(*job,1001,0);
    const auto afterWrites=f.lm.writes,afterCalls=f.entropy.calls,afterEncrypts=f.backend.encrypts;
    assert(f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,target,8402,plain.data(),plain.size(),0,false,
      nullptr,0,true,110)==-EBUSY);
    assert(f.lm.writes==afterWrites&&f.entropy.calls==afterCalls&&f.backend.encrypts==afterEncrypts&&!f.ledger.sent(8402));
    mt::Endpoint::Result result;assert(f.endpoint.takeLocalUiResult(result)&&result.id==8401&&result.kind==mt::Endpoint::Kind::Local);
    assert(!f.endpoint.takeLocalUiResult(result)&&!f.endpoint.take(0,result)&&!f.endpoint.take(1,result));
    assert(!f.runtime.submit(f.endpoint,1,target,8403,plain.data(),plain.size(),0,false,nullptr,0,true,120));
    job=f.endpoint.next(1100);assert(job&&bench::le32(job->frame.bytes)==target);f.endpoint.started(*job,0,1100,1,0);f.endpoint.complete(*job,1101,0);
    assert(f.endpoint.take(1,result)&&result.id==8403&&result.kind==mt::Endpoint::Kind::Local&&!f.endpoint.takeLocalUiResult(result));
    assert(!f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,f.peers[0].binding.node,8406,plain.data(),plain.size(),0,false,
      nullptr,0,true,125));
    job=f.endpoint.next(1120);assert(job&&bench::le32(job->frame.bytes)==f.peers[0].binding.node);
    f.endpoint.started(*job,0,1120,1,0);f.endpoint.complete(*job,1121,0);
    assert(f.endpoint.takeLocalUiResult(result)&&result.id==8406&&result.kind==mt::Endpoint::Kind::Local);
    assert(!f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,f.peers[0].binding.node,8408,plain.data(),plain.size(),0,false,
      nullptr,0,true,126));
    job=f.endpoint.next(1140);assert(job&&bench::le32(job->frame.bytes)==f.peers[0].binding.node);
    f.endpoint.started(*job,0,1140,1,0);f.endpoint.complete(*job,1141,0);
    assert(f.endpoint.takeLocalUiResult(result)&&result.id==8408&&result.kind==mt::Endpoint::Kind::Local);
    const auto finalWrites=f.lm.writes,finalCalls=f.entropy.calls,finalEncrypts=f.backend.encrypts;
    assert(f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,f.stranger.binding.node,8404,plain.data(),plain.size(),0,false,
      nullptr,0,true,130)<0);
    assert(f.lm.writes==finalWrites&&f.entropy.calls==finalCalls&&f.backend.encrypts==finalEncrypts&&!f.ledger.sent(8404));
    assert(f.runtime.revoke(target));const auto revokedWrites=f.lm.writes,revokedCalls=f.entropy.calls,revokedEncrypts=f.backend.encrypts;
    assert(f.runtime.submit(f.endpoint,mt::Endpoint::LocalUiOwner,target,8405,plain.data(),plain.size(),0,false,
      nullptr,0,true,140)<0);
    assert(f.lm.writes==revokedWrites&&f.entropy.calls==revokedCalls&&f.backend.encrypts==revokedEncrypts&&!f.ledger.sent(8405));
    pass("native-UI-owner-targets-any-active-multipin-peer-with-isolated-single-slot-result-and-preserved-client-owner");}
}
