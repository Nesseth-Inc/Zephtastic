// SPDX-License-Identifier: GPL-3.0-only
// Synthetic keys, actual shared authorities and facade. Always-quiescent is a
// test storage boundary, not evidence of a target RF barrier or live startup.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"
#include <array>

namespace {
struct Quiescent:LedgerStorageGate {bool quiescent()const override{return true;}};
struct DormantFixture {
  Key local{11},peer{97};Memory im,lm;MonocypherIdentityDeriver deriver;
  IdentityJournal identity;PkiLedger ledger;Quiescent gate;Entropy entropy;SignEntropy signing;
  Backend backend;mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonces{identity,ledger,lm,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,lm,gate};
  MultiPeerRuntime runtime{crypto,backend,identity,nonces,replay,&signing};
  PkiPeerSet set;mt::Endpoint endpoint;bench::MtCodec codec;bench::MtNodeRecord record;
  explicit DormantFixture(bool pinned=false){
    assert(!identity.load(im,deriver));IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(im,deriver,candidate,true)&&!ledger.load(lm));
    assert(!ledger.initialize(lm,local.binding.publicKey,local.binding.node,true));
    if(pinned){
      assert(!ledger.addTrustedPeer(lm,peer.binding.node,peer.binding.publicKey,true));
      PkiPeerPin pin;pin.node=peer.binding.node;memcpy(pin.publicKey,peer.binding.publicKey,32);
      assert(set.assign(local.binding,&pin,1));
    } else assert(set.assignEmpty(local.binding));
    record.setPublic();assert(codec.setKey(record.bytes[4],record.bytes+bench::MtNodeRecord::Key,16));
  }
  void noWork(){
    const auto identityBefore=im.values,ledgerBefore=lm.values;
    const auto calls=entropy.calls,signs=signing.calls,enc=backend.encrypts,dec=backend.decrypts;
    // A foreign owner's queued job proves dormant ensure() does not call the
    // global cancellation primitive. Service must separately keep RF held.
    uint8_t frame[32]{};put(frame,peer.binding.node);put(frame+4,local.binding.node);put(frame+8,8001);
    frame[15]=uint8_t(local.binding.node);frame[16]=1;
    assert(!endpoint.submit(0,frame,sizeof(frame),false,100,false,true,false,nullptr,true));
    std::array<uint8_t,sizeof(mt::Endpoint)> before{};memcpy(before.data(),&endpoint,sizeof(endpoint));
    const auto plain=data();runtime.sweep(endpoint);
    assert(runtime.submit(endpoint,0,peer.binding.node,8002,plain.data(),plain.size(),0,false,nullptr,0,true,100)==-EPERM);
    runtime.receive(endpoint,codec,frame,sizeof(frame),100,1,true);
    bench::RouteMetadata metadata;runtime.proof(endpoint,frame,sizeof(frame),metadata,100);
    PrivateEvent event;event.from=event.to=event.id=0x5a5a5a5au;event.size=91;
    memset(event.publicKey,0x5a,sizeof(event.publicKey));memset(event.data,0x5a,sizeof(event.data));
    const auto oldEvent=event;
    assert(!runtime.event(0,frame,sizeof(frame),event)&&event.from==oldEvent.from&&event.to==oldEvent.to&&
      event.id==oldEvent.id&&event.size==oldEvent.size&&!memcmp(event.publicKey,oldEvent.publicKey,sizeof(event.publicKey))&&
      !memcmp(event.data,oldEvent.data,sizeof(event.data)));
    uint8_t key[32];memset(key,0x5a,sizeof(key));
    assert(!runtime.publicIdentity(key)&&!runtime.peerPublicIdentity(peer.binding.node,key));
    for(auto b:key)assert(b==0x5a);
    bench::Frame output{};output.len=9;memset(output.bytes,0x5a,sizeof(output.bytes));const auto old=output;
    assert(!runtime.bootstrap(codec,record,127,0xffffffffu,8003,false,0,output));
    assert(!runtime.broadcast(codec,8004,plain.data(),plain.size(),output));
    assert(output.len==old.len&&!memcmp(output.bytes,old.bytes,sizeof(output.bytes)));
    assert(!memcmp(&endpoint,before.data(),sizeof(endpoint))&&endpoint.pendingPrivate(8001,peer.binding.node));
    assert(im.values==identityBefore&&lm.values==ledgerBefore&&entropy.calls==calls&&signing.calls==signs&&
      backend.encrypts==enc&&backend.decrypts==dec&&runtime.dormant(local.binding.node)&&!runtime.ready(local.binding.node));
  }
};
unsigned groups=0;
void pass(const char *s){++groups;std::cout<<"PASS "<<s<<'\n';}
}

int main(){
  {
    DormantFixture f(true);const auto before=f.set;
    for(unsigned mode=0;mode<10;++mode){
      auto invalid=f.local.binding;
      if(mode==0)invalid.version=2;
      if(mode==1)invalid.algorithm=2;
      if(mode==2)invalid.generation=0;
      if(mode==3)invalid.node=0;
      if(mode==4)invalid.node=1;
      if(mode==5)invalid.node=0xfffffffeu;
      if(mode==6)invalid.node=0xffffffffu;
      if(mode==7)invalid.node^=1;
      if(mode==8)memset(invalid.publicKey,0,32);
      if(mode==9)memset(invalid.publicKey,0xff,32);
      assert(!f.set.assignEmpty(invalid)&&!memcmp(&f.set,&before,sizeof(before)));
    }
    PkiPeerPin pin;pin.node=f.peer.binding.node;memcpy(pin.publicKey,f.peer.binding.publicKey,32);
    assert(!f.set.assign(f.local.binding,&pin,0)&&!memcmp(&f.set,&before,sizeof(before)));
    assert(f.set.assignEmpty(f.local.binding)&&!f.set.count()&&!f.set.find(pin.node)&&!f.set.at(0));
    assert(!f.set.assign(f.local.binding,&pin,0)&&!f.set.count());
    pass("assignEmpty validates complete local binding atomically while assign count-zero stays refused");
  }
  {
    DormantFixture f;const auto im=f.im.values,lm=f.lm.values;
    assert(!f.runtime.ready(f.local.binding.node)&&!f.runtime.dormant(f.local.binding.node));
    assert(f.runtime.open(f.local.binding.node,f.set)&&f.runtime.dormant(f.local.binding.node));
    assert(!f.runtime.ready(f.local.binding.node)&&!f.runtime.dormant(f.peer.binding.node)&&!f.runtime.peer());
    assert(!f.runtime.open(f.local.binding.node,f.set));
    f.noWork();assert(f.im.values==im&&f.lm.values==lm);
    PkiRuntime legacy(f.crypto,f.backend,f.identity,f.nonces,f.replay,&f.signing);
    assert(!legacy.dormant(f.local.binding.node));
    pass("verified empty domain opens once dormant with no Public/private work or global job cancellation");
  }
  {
    DormantFixture f(true);IdentityBinding binding;KeyRef key;assert(f.identity.binding(binding,key));uint32_t extra=0;
    assert(f.nonces.reserve(key,binding.publicKey,binding.node,9001,extra));
    assert(f.replay.commitAuthenticated(f.peer.binding.node,f.peer.binding.publicKey,9002));
    assert(f.runtime.open(binding.node,f.set)&&f.runtime.ready(binding.node)&&!f.runtime.dormant(binding.node));
    const auto identity=f.im.values;PkiLedger::Status before,after;assert(f.ledger.status(before));
    assert(f.runtime.revoke(f.peer.binding.node)&&f.runtime.dormant(binding.node)&&!f.runtime.ready(binding.node));
    assert(f.ledger.status(after)&&after.generation==before.generation+1&&after.sent==before.sent&&
      after.received==before.received&&after.stored==before.stored&&after.active==0&&f.ledger.sent(9001));
    assert(f.im.values==identity&&f.ledger.revoked(f.peer.binding.node));
    const auto writes=f.lm.writes;assert(f.runtime.revoke(f.peer.binding.node)&&f.lm.writes==writes);
    f.noWork();assert(f.im.values==identity);
    PkiLedger reload;assert(!reload.load(f.lm));assert(reload.sent(9001)&&reload.revoked(f.peer.binding.node));
    assert(reload.status(after)&&after.received==1&&after.sent==1&&after.stored==1&&!after.active);
    const Bytes *last=nullptr;
    for(const auto &entry:f.lm.values)if(!last||bench::le32(entry.second.data()+8)>bench::le32(last->data()+8))last=&entry.second;
    assert(last&&last->at(PkiLedger::PeerOffset+36)==1&&
      bench::le32(last->data()+PkiLedger::PeerOffset+40)==9002);
    pass("last-peer revoke becomes dormant and preserves identity, global nonce and exact durable replay history");
  }
  {
    DormantFixture f(true);Key other{149};PkiPeerPin pins[2]{};
    pins[0].node=f.peer.binding.node;memcpy(pins[0].publicKey,f.peer.binding.publicKey,32);
    pins[1].node=other.binding.node;memcpy(pins[1].publicKey,other.binding.publicKey,32);
    assert(!f.ledger.addTrustedPeer(f.lm,other.binding.node,other.binding.publicKey,true));
    assert(f.set.assign(f.local.binding,pins,2)&&f.runtime.open(f.local.binding.node,f.set));
    const auto identity=f.im.values;
    assert(f.runtime.revoke(f.peer.binding.node)&&f.runtime.ready(f.local.binding.node)&&!f.runtime.dormant(f.local.binding.node));
    uint8_t key[32]{};assert(f.runtime.peerPublicIdentity(other.binding.node,key)&&!memcmp(key,other.binding.publicKey,32));
    const auto plain=data();assert(!f.runtime.submit(f.endpoint,0,other.binding.node,9201,
      plain.data(),plain.size(),0,false,nullptr,0,true,100));
    const auto writes=f.lm.writes,calls=f.entropy.calls,enc=f.backend.encrypts;
    assert(f.runtime.submit(f.endpoint,0,f.peer.binding.node,9202,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
    f.runtime.sweep(f.endpoint);
    assert(f.endpoint.pendingPrivate(9201,other.binding.node)&&f.im.values==identity&&
      f.lm.writes==writes&&f.entropy.calls==calls&&f.backend.encrypts==enc);
    pass("non-last revoke keeps the other peer ready with its exact queued ciphertext and no rejected-peer nonce use");
  }
  for(unsigned mode=0;mode<5;++mode){
    DormantFixture f;assert(f.runtime.open(f.local.binding.node,f.set));
    uint8_t wire[17]{};put(wire,f.peer.binding.node);put(wire+4,f.local.binding.node);put(wire+8,9100);
    assert(!f.endpoint.submit(0,wire,sizeof(wire),false,100,false,true,false,nullptr,true));
    if(mode==0)assert(f.backend.real.close());
    if(mode==1)assert(f.identity.close());
    if(mode==2)assert(f.ledger.close());
    if(mode==3){auto changed=f.local.binding;++changed.generation;IdentityCandidate c(changed,f.local.scalar);
      assert(!f.identity.store(f.im,f.deriver,c,true));}
    if(mode==4)f.runtime.freeze();
    assert(!f.runtime.dormant(f.local.binding.node)&&!f.runtime.ready(f.local.binding.node));
    f.runtime.sweep(f.endpoint);mt::Endpoint::Result result;
    assert(f.endpoint.take(0,result)&&result.kind==mt::Endpoint::Kind::Failed&&result.id==9100);
    f.runtime.sweep(f.endpoint);assert(!f.endpoint.take(0,result));
    assert(!f.runtime.open(f.local.binding.node,f.set));
  }
  pass("closed backend identity replay, changed identity generation and explicit freeze never masquerade dormant");
  {
    DormantFixture f;auto broken=f.lm.values;broken.begin()->second[0]^=1;
    Memory memory;memory.values=broken;PkiLedger corrupt;assert(corrupt.load(memory)==-EILSEQ);
    LedgerReplayAuthority replay(f.identity,corrupt,memory,f.gate);
    MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,f.nonces,replay,&f.signing);
    assert(!runtime.open(f.local.binding.node,f.set)&&!runtime.dormant(f.local.binding.node));
    assert(!runtime.open(f.local.binding.node,f.set));
    pass("corrupt replay domain cannot open dormant or reopen a failed object");
  }
  {
    DormantFixture f;const auto im=f.im.values,lm=f.lm.values;
    assert(!f.runtime.open(f.peer.binding.node,f.set)&&!f.runtime.dormant(f.peer.binding.node));
    assert(!f.runtime.open(f.local.binding.node,f.set)&&f.im.values==im&&f.lm.values==lm);
    pass("wrong local domain refuses before trust or history mutation and cannot reopen");
  }
  std::cout<<"{\"PASS\":true,\"groups\":"<<groups<<",\"hardware\":false,\"synthetic_credentials_only\":true}\n";
}
