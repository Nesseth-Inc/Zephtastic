// SPDX-License-Identifier: GPL-3.0-only
// Focused MultiPeerRuntime live-pin publication tests. Synthetic keys and an
// explicit 16-contact durable-directory model; this does not exercise Service
// RX provenance or production NVS contact persistence.
#include "pki_runtime_fixture.h"
#include "pki/MultiPeerRuntime.h"

namespace {
struct AdoptionGate final : LedgerStorageGate { bool quiescent() const override { return true; } };
// PkiLedger is intentionally retained only for the real local identity-bound
// TX nonce authority below: its legacy peer capacity is four. MultiPeerRuntime
// publication needs a separate durable-directory contract model to exercise
// the production 16-contact bound without pretending that the legacy ledger
// can store sixteen contacts.
struct TestDurablePeerDirectory final : PeerReplayAuthority {
  struct Entry {PkiPeerPin pin{};bool active=false,revoked=false;uint32_t ids[64]{};unsigned idCount=0;};
  IdentityBinding local{};Entry entries[PkiPeerSet::Limit]{};size_t count=0;unsigned writes=0;
  bool bind(const IdentityBinding &value){local=value;return true;}
  bool add(const PkiPeerPin &pin){
    if(count>=PkiPeerSet::Limit||pin.node<=1||pin.node>=0xfffffffeu)return false;
    for(size_t i=0;i<count;++i)if(entries[i].pin.node==pin.node||!memcmp(entries[i].pin.publicKey,pin.publicKey,32))return false;
    entries[count].pin=pin;entries[count].active=true;++count;++writes;return true;
  }
  int index(uint32_t node)const{for(size_t i=0;i<count;++i)if(entries[i].pin.node==node)return int(i);return -1;}
  bool boundTo(const uint8_t p[32],uint32_t n)const override{return p&&n==local.node&&!memcmp(p,local.publicKey,32);}
  bool lookup(uint32_t n,uint8_t p[32])const override{const int i=index(n);if(i<0||!p)return false;memcpy(p,entries[i].pin.publicKey,32);return true;}
  bool active(uint32_t n,const uint8_t p[32])const override{const int i=index(n);return p&&i>=0&&entries[i].active&&!entries[i].revoked&&!memcmp(p,entries[i].pin.publicKey,32);}
  ReplayCheck seen(uint32_t n,const uint8_t p[32],uint32_t id)const override{
    const int i=index(n);if(i<0||entries[i].revoked)return ReplayCheck::UnknownPeer;
    if(!p||memcmp(p,entries[i].pin.publicKey,32))return ReplayCheck::WrongKey;
    for(unsigned j=0;j<entries[i].idCount;++j)if(entries[i].ids[j]==id)return ReplayCheck::Seen;
    return id?ReplayCheck::Fresh:ReplayCheck::Invalid;
  }
  bool commitAuthenticated(uint32_t n,const uint8_t p[32],uint32_t id)override{
    const int i=index(n);if(!id||!active(n,p)||entries[i].idCount==64||seen(n,p,id)!=ReplayCheck::Fresh)return false;
    entries[i].ids[entries[i].idCount++]=id;++writes;return true;
  }
  bool revoked(uint32_t n)const override{const int i=index(n);return i>=0&&entries[i].revoked;}
  bool revokeTrustedPeer(uint32_t n,const uint8_t p[32])override{
    const int i=index(n);if(i<0||!p||memcmp(p,entries[i].pin.publicKey,32)||entries[i].revoked)return false;
    entries[i].revoked=true;entries[i].active=false;++writes;return true;
  }
};
struct FailingActive final : PeerReplayAuthority {
  TestDurablePeerDirectory &inner;uint32_t failNode=0;mutable unsigned lookupCalls=0;unsigned failLookupAt=0;
  explicit FailingActive(TestDurablePeerDirectory &v):inner(v){}
  bool boundTo(const uint8_t p[32],uint32_t n)const override{return inner.boundTo(p,n);}
  bool lookup(uint32_t n,uint8_t p[32])const override {
    if(n==failNode&&++lookupCalls==failLookupAt)return false;
    return inner.lookup(n,p);
  }
  bool active(uint32_t n,const uint8_t p[32])const override{return inner.active(n,p);}
  ReplayCheck seen(uint32_t n,const uint8_t p[32],uint32_t id)const override{return inner.seen(n,p,id);}
  bool commitAuthenticated(uint32_t n,const uint8_t p[32],uint32_t id)override{return inner.commitAuthenticated(n,p,id);}
  bool revoked(uint32_t n)const override{return inner.revoked(n);}
  bool revokeTrustedPeer(uint32_t n,const uint8_t p[32])override{return inner.revokeTrustedPeer(n,p);}
};
struct AdoptionFixture {
  Key local{11};
  std::vector<Key> peers;
  Memory identityMemory, ledgerMemory;
  MonocypherIdentityDeriver deriver;
  IdentityJournal identity;
  PkiLedger ledger;
  TestDurablePeerDirectory replay;
  AdoptionGate gate;
  Entropy entropy;
  SignEntropy signing;
  Backend backend;
  mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,ledgerMemory,entropy,gate};
  MultiPeerRuntime runtime{crypto,backend,identity,nonce,replay,&signing};
  PkiPeerSet initial;
  mt::Endpoint endpoint;

  explicit AdoptionFixture(unsigned count=1) {
    assert(count<=PkiPeerSet::Limit);
    peers.reserve(PkiPeerSet::Limit+1);
    for(unsigned i=0;i<=PkiPeerSet::Limit;++i)peers.emplace_back(97+i*53);
    assert(!identity.load(identityMemory,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(identityMemory,deriver,candidate,true));
    assert(!ledger.load(ledgerMemory));
    assert(!ledger.initialize(ledgerMemory,local.binding.publicKey,local.binding.node,true));
    assert(replay.bind(local.binding));
    PkiPeerPin pins[PkiPeerSet::Limit]{};
    for(unsigned i=0;i<count;++i) {
      pins[i].node=peers[i].binding.node;
      memcpy(pins[i].publicKey,peers[i].binding.publicKey,32);
      assert(replay.add(pins[i]));
    }
    if(count) assert(initial.assign(local.binding,pins,count));
    else assert(initial.assignEmpty(local.binding));
    assert(runtime.open(local.binding.node,initial));
  }
  PkiPeerPin pin(unsigned i) const {
    PkiPeerPin out{};out.node=peers[i].binding.node;
    memcpy(out.publicKey,peers[i].binding.publicKey,32);return out;
  }
  void persist(unsigned i) {
    auto p=pin(i);assert(replay.add(p));
  }
};
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
}

int main() {
  {
    AdoptionFixture f(1);
    const auto plain=data("hot child remains untouched");
    assert(!f.runtime.submit(f.endpoint,0,f.peers[0].binding.node,70,plain.data(),plain.size(),0,
                             true,nullptr,0,true,100));
    const unsigned queuedFree=f.endpoint.free();
    f.persist(1);
    const auto directoryWrites=f.replay.writes,nonceWrites=f.ledgerMemory.writes;
    const auto p=f.pin(1);
    assert(f.runtime.adoptPersistedPeer(p));
    assert(f.replay.writes==directoryWrites&&f.ledgerMemory.writes==nonceWrites&&f.endpoint.free()==queuedFree&&
           f.endpoint.hasPrivatePeerWork(f.peers[0].binding.node));
    uint8_t key[32]{};
    assert(f.runtime.peerPublicIdentity(p.node,key)&&!memcmp(key,p.publicKey,32));
    assert(f.runtime.ready(f.local.binding.node));
    assert(!f.runtime.submit(f.endpoint,0,f.peers[0].binding.node,71,plain.data(),plain.size(),0,
                             false,nullptr,0,true,101));
    assert(!f.runtime.submit(f.endpoint,0,p.node,72,plain.data(),plain.size(),0,
                             false,nullptr,0,true,102));
    assert(!f.runtime.adoptPersistedPeer(p)); // Existing pins are never republished.
    pass("adoption-preserves-hot-peer-work-and-adds-cold-destination-without-store-write");
  }
  {
    AdoptionFixture f;
    const auto p=f.pin(1);
    assert(!f.runtime.adoptPersistedPeer(p)); // No durable record.
    f.persist(1);
    auto wrong=p;wrong.publicKey[0]^=0x80;
    assert(!f.runtime.adoptPersistedPeer(wrong));
    assert(!f.runtime.peerPublicIdentity(p.node,wrong.publicKey));
    pass("absent-or-key-mismatched-durable-record-is-not-published");
  }
  {
    AdoptionFixture f;
    f.persist(1);const auto p=f.pin(1);
    assert(f.runtime.adoptPersistedPeer(p));
    assert(f.runtime.revoke(p.node));
    assert(!f.runtime.adoptPersistedPeer(p));
    uint8_t key[32]{};
    assert(!f.runtime.peerPublicIdentity(p.node,key));
    pass("revoked-contact-slot-cannot-be-revived-by-adoption");
  }
  {
    AdoptionFixture f(0);
    assert(f.runtime.dormant(f.local.binding.node));
    f.persist(0);const auto p=f.pin(0);
    assert(f.runtime.adoptPersistedPeer(p));
    assert(f.runtime.ready(f.local.binding.node));
    uint8_t key[32]{};
    assert(f.runtime.peerPublicIdentity(p.node,key)&&!memcmp(key,p.publicKey,32));
    pass("first-adoption-seeds-one-child-and-transitions-dormant-runtime");
  }
  {
    AdoptionFixture f(0);FailingActive flaky{f.replay};
    MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,f.nonce,flaky,&f.signing);
    assert(runtime.open(f.local.binding.node,f.initial));
    f.persist(0);const auto p=f.pin(0);const auto writes=f.replay.writes,nonceWrites=f.ledgerMemory.writes;
    flaky.failNode=p.node;flaky.failLookupAt=2;
    assert(!runtime.adoptPersistedPeer(p));
    assert(f.replay.writes==writes&&f.ledgerMemory.writes==nonceWrites&&runtime.dormant(f.local.binding.node));
    uint8_t key[32]{};assert(!runtime.peerPublicIdentity(p.node,key));
    flaky.failLookupAt=0;
    assert(runtime.adoptPersistedPeer(p)&&runtime.ready(f.local.binding.node));
    pass("failed-first-child-readiness-rolls-back-pin-and-can-retry-cleanly");
  }
  {
    AdoptionFixture f(PkiPeerSet::Limit-1);
    f.persist(PkiPeerSet::Limit-1);
    const auto nonceWrites=f.ledgerMemory.writes;
    assert(f.runtime.adoptPersistedPeer(f.pin(PkiPeerSet::Limit-1)));
    assert(f.runtime.ready(f.local.binding.node));
    const auto writes=f.replay.writes;
    assert(!f.runtime.adoptPersistedPeer(f.pin(PkiPeerSet::Limit)));
    assert(f.replay.writes==writes&&f.ledgerMemory.writes==nonceWrites&&f.runtime.ready(f.local.binding.node));
    pass("sixteenth-adoption-reaches-contact-limit-and-refuses-seventeenth-without-mutation");
  }
  {
    struct BasicNonce final : NonceAuthority {
      bool reserve(KeyRef,const uint8_t *,uint32_t,uint32_t,uint32_t &extra)override {
        extra=0x12345678u;return true;
      }
    } basic;
    AdoptionFixture f(4);
    MultiPeerRuntime runtime(f.crypto,f.backend,f.identity,basic,f.replay,&f.signing);
    assert(runtime.open(f.local.binding.node,f.initial));
    f.persist(4);const auto writes=f.replay.writes,nonceWrites=f.ledgerMemory.writes;
    assert(!runtime.adoptPersistedPeer(f.pin(4)));
    assert(f.replay.writes==writes&&f.ledgerMemory.writes==nonceWrites&&runtime.ready(f.local.binding.node));
    uint8_t key[32]{};
    assert(!runtime.peerPublicIdentity(f.peers[4].binding.node,key));
    pass("nonpersistent-nonce-provider-refuses-growth-past-four-peers");
  }
  return 0;
}
