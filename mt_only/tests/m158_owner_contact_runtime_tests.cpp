// SPDX-License-Identifier: GPL-3.0-only
// Owner-authorized runtime adoption, replacement, and soft-forget churn.
#define main m155_runtime_peer_adoption_baseline_main
#include "m155_runtime_peer_adoption_tests.cpp"
#undef main
#include <memory>

namespace {
struct SoftForgetDirectory final:PeerReplayAuthority {
  TestDurablePeerDirectory &inner;
  explicit SoftForgetDirectory(TestDurablePeerDirectory &value):inner(value){}
  bool add(const PkiPeerPin &pin){return inner.add(pin);}
  bool replace(const PkiPeerPin &pin){
    const int at=inner.index(pin.node);if(at<0)return false;
    inner.entries[unsigned(at)].pin=pin;inner.entries[unsigned(at)].active=true;
    inner.entries[unsigned(at)].revoked=false;++inner.writes;return true;
  }
  bool boundTo(const uint8_t p[32],uint32_t n)const override{return inner.boundTo(p,n);}
  bool lookup(uint32_t n,uint8_t p[32])const override{return inner.lookup(n,p);}
  bool active(uint32_t n,const uint8_t p[32])const override{return inner.active(n,p);}
  ReplayCheck seen(uint32_t n,const uint8_t p[32],uint32_t id)const override{return inner.seen(n,p,id);}
  bool commitAuthenticated(uint32_t n,const uint8_t p[32],uint32_t id)override{return inner.commitAuthenticated(n,p,id);}
  bool revoked(uint32_t)const override{return false;}
  bool revokeTrustedPeer(uint32_t n,const uint8_t p[32])override{
    const int at=inner.index(n);if(at<0||!p||!inner.entries[unsigned(at)].active||
       memcmp(inner.entries[unsigned(at)].pin.publicKey,p,32))return false;
    for(size_t i=unsigned(at);i+1<inner.count;++i)inner.entries[i]=inner.entries[i+1];
    inner.entries[--inner.count]={};++inner.writes;return true;
  }
};

void ownerAliasReplacePreservesSiblingWork(){
  AdoptionFixture f(1);SoftForgetDirectory directory{f.replay};
  MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,f.nonce,directory,&f.signing);
  assert(runtime.open(f.local.binding.node,f.initial));
  const auto sibling=f.peers[0].binding.node;

  constexpr uint32_t Alias=0x33334444u;
  auto alias=f.pin(1);alias.node=Alias;
  assert(directory.add(alias));
  auto unpersisted=alias;memcpy(unpersisted.publicKey,f.peers[2].binding.publicKey,32);
  assert(!runtime.adoptPersistedPeer(unpersisted));
  uint8_t observed[32]{};assert(!runtime.peerPublicIdentity(Alias,observed));
  const unsigned writes=directory.inner.writes,nonceWrites=f.ledgerMemory.writes;
  assert(runtime.adoptPersistedPeer(alias));
  assert(directory.inner.writes==writes&&f.ledgerMemory.writes==nonceWrites);
  assert(runtime.ready(f.local.binding.node)&&runtime.peerPublicIdentity(Alias,observed)&&
         !memcmp(observed,alias.publicKey,32));
  // The public API deliberately refuses a duplicate direct publication; the
  // already-published session and directory remain intact and write-free.
  const unsigned freeAfterAdoption=f.endpoint.free();
  assert(!runtime.adoptPersistedPeer(alias)&&runtime.peerPublicIdentity(Alias,observed));
  assert(directory.inner.writes==writes&&f.ledgerMemory.writes==nonceWrites&&
         f.endpoint.free()==freeAfterAdoption);

  // Make the alias child hot, then transmit and consume its isolated
  // Endpoint job and the sibling job before replacing the durable alias key.
  const auto plain=data("runtime alias session");
  assert(!runtime.submit(f.endpoint,0,sibling,901,plain.data(),plain.size(),0,false,nullptr,0,true,100));
  assert(!runtime.submit(f.endpoint,0,Alias,904,plain.data(),plain.size(),0,false,nullptr,0,true,101));
  assert(f.endpoint.hasPrivatePeerWork(sibling)&&f.endpoint.hasPrivatePeerWork(Alias));
  bool sentSibling=false,sentAlias=false;
  for(unsigned i=0;i<2;++i){
    auto *job=f.endpoint.next(1000+i*10);assert(job&&job->privateMessage);
    const uint32_t destination=bench::le32(job->frame.bytes);
    const uint32_t id=bench::le32(job->frame.bytes+8);
    assert((destination==sibling&&id==901)||(destination==Alias&&id==904));
    f.endpoint.started(*job,0,1000+i*10,100,0);
    f.endpoint.complete(*job,1001+i*10,0);
    if(destination==sibling)sentSibling=true;else sentAlias=true;
  }
  assert(sentSibling&&sentAlias&&!f.endpoint.hasPrivatePeerWork(sibling)&&
         !f.endpoint.hasPrivatePeerWork(Alias));
  // Keep unrelated accepted work queued while the target child is rebuilt.
  assert(!runtime.submit(f.endpoint,0,sibling,905,plain.data(),plain.size(),0,false,nullptr,0,true,2000));
  assert(f.endpoint.hasPrivatePeerWork(sibling));

  Key replacementKey{231};auto replacement=alias;
  memcpy(replacement.publicKey,replacementKey.binding.publicKey,32);
  assert(directory.replace(replacement));
  const unsigned replacementWrites=directory.inner.writes,nonceWritesBeforeReplacement=f.ledgerMemory.writes;
  const unsigned freeBeforeReplacement=f.endpoint.free();
  assert(runtime.adoptPersistedPeer(replacement));
  assert(directory.inner.writes==replacementWrites&&
         f.ledgerMemory.writes==nonceWritesBeforeReplacement&&
         f.endpoint.free()==freeBeforeReplacement&&f.endpoint.hasPrivatePeerWork(sibling));
  assert(runtime.peerPublicIdentity(Alias,observed)&&!memcmp(observed,replacement.publicKey,32));
  assert(!runtime.acceptsPeerKey(Alias,alias.publicKey,32)&&
         runtime.acceptsPeerKey(Alias,replacement.publicKey,32));
  assert(f.endpoint.hasPrivatePeerWork(sibling));
  assert(!runtime.submit(f.endpoint,0,sibling,902,plain.data(),plain.size(),0,false,nullptr,0,true,2001));
  assert(!runtime.submit(f.endpoint,0,Alias,903,plain.data(),plain.size(),0,false,nullptr,0,true,2002));
  assert(runtime.ready(f.local.binding.node)&&f.endpoint.hasPrivatePeerWork(sibling));
}

void softForgetCanRelearnWithoutSlotLeak(){
  AdoptionFixture f(0);SoftForgetDirectory directory{f.replay};
  MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,f.nonce,directory,&f.signing);
  assert(runtime.open(f.local.binding.node,f.initial)&&runtime.dormant(f.local.binding.node));
  std::vector<std::unique_ptr<Key>> keys;keys.reserve(24);
  for(unsigned i=0;i<24;++i)keys.emplace_back(new Key(30+i));
  for(unsigned i=0;i<keys.size();++i){
    PkiPeerPin pin{};pin.node=0x50000000u+i;memcpy(pin.publicKey,keys[i]->binding.publicKey,32);
    assert(directory.add(pin));assert(runtime.adoptPersistedPeer(pin));
    assert(runtime.ready(f.local.binding.node));
    uint8_t observed[32]{};assert(runtime.peerPublicIdentity(pin.node,observed)&&!memcmp(observed,pin.publicKey,32));
    assert(runtime.revoke(pin.node));
    assert(directory.inner.count==0&&!directory.revoked(pin.node));
    assert(runtime.dormant(f.local.binding.node)&&!runtime.peerPublicIdentity(pin.node,observed));
  }
}

void hardRevocationStaysBlocked(){
  AdoptionFixture f(0);MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,f.nonce,f.replay,&f.signing);
  assert(runtime.open(f.local.binding.node,f.initial)&&runtime.dormant(f.local.binding.node));
  f.persist(0);const auto pin=f.pin(0);
  assert(runtime.adoptPersistedPeer(pin)&&runtime.ready(f.local.binding.node));
  assert(runtime.revoke(pin.node)&&f.replay.revoked(pin.node));
  assert(runtime.dormant(f.local.binding.node)&&!runtime.adoptPersistedPeer(pin));
  uint8_t observed[32]{};
  assert(f.replay.revoked(pin.node)&&!runtime.peerPublicIdentity(pin.node,observed));
}

void nonpersistentProviderRefusesRemovalAndReplacement(){
  struct BasicNonce final:NonceAuthority {
    bool reserve(KeyRef,const uint8_t *,uint32_t,uint32_t,uint32_t &extra)override{extra=77;return true;}
  } basic;
  AdoptionFixture f(1);MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,basic,f.replay,&f.signing);
  assert(runtime.open(f.local.binding.node,f.initial));
  const auto pin=f.pin(0);const unsigned writes=f.replay.writes,nonceWrites=f.ledgerMemory.writes;
  uint8_t observed[32]{};
  assert(!runtime.revoke(pin.node));
  assert(f.replay.writes==writes&&f.ledgerMemory.writes==nonceWrites&&
         runtime.ready(f.local.binding.node)&&runtime.peerPublicIdentity(pin.node,observed)&&
         !memcmp(observed,pin.publicKey,32));

  // Model an owner replacement already committed to the directory. A
  // nonpersistent provider cannot discard the child's RAM attempted-ID history.
  Key replacementKey{231};auto replacement=pin;
  memcpy(replacement.publicKey,replacementKey.binding.publicKey,32);
  const int slot=f.replay.index(pin.node);assert(slot>=0);
  f.replay.entries[unsigned(slot)].pin=replacement;++f.replay.writes;
  const unsigned afterStorageCommit=f.replay.writes,nonceAfterStorageCommit=f.ledgerMemory.writes;
  assert(!runtime.adoptPersistedPeer(replacement));
  assert(f.replay.writes==afterStorageCommit&&f.ledgerMemory.writes==nonceAfterStorageCommit);
  assert(!runtime.acceptsPeerKey(pin.node,replacement.publicKey,32)&&
         runtime.acceptsPeerKey(pin.node,pin.publicKey,32)&&
         !runtime.peerPublicIdentity(pin.node,observed));
}
}

int main(){
  ownerAliasReplacePreservesSiblingWork();
  softForgetCanRelearnWithoutSlotLeak();
  hardRevocationStaysBlocked();
  nonpersistentProviderRefusesRemovalAndReplacement();
  std::cout<<"PASS M158 owner-contact runtime adoption/replacement/forget"<<'\n';
  return 0;
}
