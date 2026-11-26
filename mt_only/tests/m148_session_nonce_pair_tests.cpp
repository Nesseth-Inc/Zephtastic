// SPDX-License-Identifier: GPL-3.0-only
// Real host PSA CCM, synthetic public fixture keys and memory counter records.
#include "pki_runtime_fixture.h"
#include "pki/PkiNonceCounter.h"
#include <array>

namespace {
struct CounterStore final : PkiNonceCounterStore {
  std::array<Bytes,2> slots;
  int read(unsigned slot,uint8_t *out,size_t size) override {
    assert(slot<2&&size==PkiNonceCounter::RecordBytes);
    if(slots[slot].empty())return -ENOENT;
    assert(slots[slot].size()==size);memcpy(out,slots[slot].data(),size);return int(size);
  }
  int write(unsigned slot,const uint8_t *bytes,size_t size) override {
    assert(slot<2&&size==PkiNonceCounter::RecordBytes);
    slots[slot]={bytes,bytes+size};return 0;
  }
};
struct Keys final : KeyProvider {
  const Key &key;KeyRef ref{0,1};
  explicit Keys(const Key &k):key(k){}
  bool active(KeyRef r) const override{return r.slot==ref.slot&&r.generation==ref.generation;}
  bool withPrivate(KeyRef r,PrivateKeyOperation &op) override {
    return active(r)&&op.apply(key.scalar);
  }
};
struct Pairs final : NonceAuthority {
  PkiNonceCounter &counter;const Key &key;unsigned calls=0;
  Pairs(PkiNonceCounter &c,const Key &k):counter(c),key(k){}
  bool persistentNoncePairs() const override{return true;}
  bool reserve(KeyRef,const uint8_t pub[32],uint32_t node,uint32_t id,uint32_t &extra) override {
    ++calls;
    assert(id&&node==key.binding.node&&!memcmp(pub,key.binding.publicKey,32));
    return counter.reserve(extra)==0;
  }
};
uint32_t extra(const PkiFrame &frame){
  assert(frame.size>=12);uint32_t v=0;
  for(unsigned i=0;i<4;++i)v|=uint32_t(frame.bytes[frame.size-4+i])<<(8*i);
  return v;
}
}

int main(){
  Key a(13),b(61);Keys ka(a),kb(b);Backend backend;mt::experimental::PkiCrypto crypto(backend);
  CounterStore store;PkiNonceCounterBinding binding;
  binding.node=a.binding.node;memcpy(binding.publicKey,a.binding.publicKey,32);
  binding.legacyGeneration=2653;binding.legacyCrc=0x12345678;
  PkiNonceCounter counter(store);assert(!counter.initialize(binding,true)&&!counter.open(binding,true));
  Pairs pairs(counter,a),unused(counter,b);
  const uint8_t data[]={8,1,18,3,'a','b','c'};
  PkiFrame first{},retry{},second{};uint8_t plain[227]{};size_t n=0;
  {
    PkiSession alice(crypto,ka,pairs),bob(crypto,kb,unused);
    assert(alice.open(ka.ref,a.binding.node,a.binding.publicKey,b.binding.node,b.binding.publicKey));
    assert(bob.open(kb.ref,b.binding.node,b.binding.publicKey,a.binding.node,a.binding.publicKey));
    assert(alice.send(42,data,sizeof(data),first)==SessionResult::Ok&&extra(first)==1);
    const unsigned calls=pairs.calls,encrypts=backend.encrypts;
    assert(alice.send(42,data,sizeof(data),second)==SessionResult::NonceReused);
    assert(pairs.calls==calls&&backend.encrypts==encrypts);
    assert(alice.retry(42,retry)==SessionResult::Ok&&retry.size==first.size);
    assert(!memcmp(retry.bytes,first.bytes,first.size)&&pairs.calls==calls&&backend.encrypts==encrypts);
    assert(bob.authenticate(first,plain,sizeof(plain),n)==SessionResult::Ok&&n==sizeof(data)&&!memcmp(data,plain,n));
    assert(alice.retire(42));
    assert(alice.send(42,data,sizeof(data),second)==SessionResult::Ok&&extra(second)==2);
    assert(memcmp(first.bytes,second.bytes,first.size));
    assert(bob.authenticate(second,plain,sizeof(plain),n)==SessionResult::Ok&&n==sizeof(data)&&!memcmp(data,plain,n));
    assert(alice.retire(42));
    // Capacity no longer stops at the basic session's 32 burned IDs.
    for(uint32_t i=0;i<40;++i){
      assert(alice.send(100+i,data,sizeof(data),retry)==SessionResult::Ok);
      assert(extra(retry)==3+i&&alice.retire(100+i));
    }
  }
  counter.close();PkiNonceCounter rebooted(store);assert(!rebooted.open(binding,true));
  Pairs after(rebooted,a);PkiSession restarted(crypto,ka,after);
  assert(restarted.open(ka.ref,a.binding.node,a.binding.publicKey,b.binding.node,b.binding.publicKey));
  assert(restarted.send(42,data,sizeof(data),retry)==SessionResult::Ok&&extra(retry)==257);
  assert(memcmp(first.bytes,retry.bytes,first.size)&&memcmp(second.bytes,retry.bytes,second.size));
  assert(crypto.decrypt(b.scalar,a.binding.publicKey,a.binding.node,42,retry.bytes,retry.size,plain,sizeof(plain),n));
  assert(n==sizeof(data)&&!memcmp(data,plain,n));
  puts("PASS M148 PSA session pending-ID guard, cached retry, retired-ID nonce pairs and reboot burn");
}
