// SPDX-License-Identifier: GPL-3.0-only
#include "PkiSession.h"
#include "monocypher.h"
#include <cassert>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <set>
#include <tuple>
#include <array>
using namespace mt::pki;
using mt::experimental::PkiBackend;
using mt::experimental::PkiCrypto;
unsigned pkiWipeCalls=0,pkiWipe32=0,pkiWipe235=0;
extern "C" void mt_pki_test_wipe(void *p,size_t n) {
  crypto_wipe(p,n);
  const auto *b=static_cast<const uint8_t *>(p);
  assert(std::all_of(b,b+n,[](uint8_t x){return x==0;}));
  ++pkiWipeCalls;if(n==32)++pkiWipe32;if(n==235)++pkiWipe235;
}
namespace {
struct Keys:KeyProvider {
  uint8_t scalar[32]{};KeyRef ref{0,1};bool enabled=true,fail=false,wrong=false;
  unsigned reads=0;
  explicit Keys(const uint8_t *s){memcpy(scalar,s,32);}
  bool active(KeyRef r)const override{return enabled&&r.slot==ref.slot&&r.generation==ref.generation;}
  bool withPrivate(KeyRef r,PrivateKeyOperation &op)override {
    ++reads;if(!active(r))return false;
    uint8_t tmp[32];memcpy(tmp,scalar,32);if(wrong)tmp[0]^=8;
    const bool result=op.apply(tmp);crypto_wipe(tmp,32);return result&&!fail;
  }
};
struct Nonces:NonceAuthority {
  std::set<std::tuple<std::array<uint8_t,32>,uint32_t,uint32_t>> used;
  unsigned calls=0;bool fail=false;
  bool reserve(KeyRef,const uint8_t pub[32],uint32_t from,uint32_t id,uint32_t &extra)override {
    ++calls;std::array<uint8_t,32> domain{};memcpy(domain.data(),pub,32);
    const bool fresh=used.emplace(domain,from,id).second;
    extra=0x10987654u+calls*0x10203u; // PUBLIC deterministic fixture, host only.
    return fresh&&!fail;
  }
};
struct FailBackend:PkiBackend {
  PkiBackend &b;bool fail=false;unsigned derivations=0;
  explicit FailBackend(PkiBackend &source):b(source){}
  bool sha256(const uint8_t *p,size_t n,uint8_t o[32])override{++derivations;return b.sha256(p,n,o);}
  bool hmac256(const uint8_t k[32],const uint8_t *p,size_t n,uint8_t o[32])override{return b.hmac256(k,p,n,o);}
  bool ccm(bool e,const uint8_t k[32],const uint8_t iv[13],const uint8_t *p,size_t n,uint8_t *o,uint8_t t[8])override {
    if(fail){memset(o,0xa7,n);return false;}return b.ccm(e,k,iv,p,n,o,t);
  }
};
void untouched(const void *p,size_t n){const auto *b=static_cast<const uint8_t *>(p);assert(std::all_of(b,b+n,[](uint8_t x){return x==0x5a;}));}
template<class T>void poison(T &value){auto *p=reinterpret_cast<uint8_t *>(&value);std::fill(p,p+sizeof(value),uint8_t(0x5a));}
constexpr uint32_t Alice=0x12345678,Bob=0x87654321;
}
void runSessionTests(PkiBackend &backend,const uint8_t *a,const uint8_t *b,const uint8_t *A,const uint8_t *B) {
  PkiCrypto crypto(backend);Keys ka(a),kb(b);Nonces na,nb;
  PkiSession alice(crypto,ka,na),bob(crypto,kb,nb);
  assert(alice.open(ka.ref,Alice,A,Bob,B) && bob.open(kb.ref,Bob,B,Alice,A));
  uint8_t data[227],plain[227];for(unsigned i=0;i<227;++i)data[i]=uint8_t(31+i*13);
  PkiFrame frame{},retry{};size_t len=0;unsigned cases=0;
  {
    FailBackend counted(backend);PkiCrypto primitive(counted);Keys keys(a);Nonces n;
    PkiSession first(primitive,keys,n);assert(first.open(keys.ref,Alice,A,Bob,B));
    assert(first.send(701,data,8,frame)==SessionResult::Ok&&counted.derivations==1);
    uint8_t route[]={0x18,0};uint8_t signedAck[233]{};size_t ackSize=0;
    assert(first.attachProof(701,route,sizeof(route),signedAck,sizeof(signedAck),ackSize)&&counted.derivations==1);
    assert(first.send(702,data,8,frame)==SessionResult::Ok&&counted.derivations==1);
    keys.wrong=true;assert(first.send(703,data,8,frame)==SessionResult::KeyUnavailable);keys.wrong=false;
    keys.enabled=false;assert(first.send(704,data,8,frame)==SessionResult::NotReady);first.close();
    keys.enabled=true;++keys.ref.generation;
    PkiSession second(primitive,keys,n);assert(second.open(keys.ref,Alice,A,Bob,B));
    assert(second.send(705,data,8,frame)==SessionResult::Ok&&counted.derivations==2);
    uint8_t raw[239];size_t size=0;
    assert(primitive.encrypt(a,B,Alice,706,1,data,8,raw,sizeof(raw),size)&&counted.derivations==3);
    assert(primitive.encrypt(a,B,Alice,707,1,data,8,raw,sizeof(raw),size)&&counted.derivations==4);
  }
  for(size_t size:{size_t(1),size_t(16),size_t(100),size_t(227)}) {
    const uint32_t id=uint32_t(size);
    assert(alice.send(id,data,size,frame)==SessionResult::Ok);
    assert(frame.from==Alice&&frame.to==Bob&&frame.id==id&&frame.size==size+12);
    const unsigned reads=ka.reads,calls=na.calls;
    assert(alice.retry(id,retry)==SessionResult::Ok && !memcmp(frame.bytes,retry.bytes,frame.size));
    assert(retry.from==frame.from&&retry.to==frame.to&&retry.id==id&&retry.size==frame.size);
    assert(ka.reads==reads&&na.calls==calls);
    assert(bob.receive(frame,plain,227,len)==SessionResult::Ok&&len==size&&!memcmp(plain,data,size));
    memset(plain,0x5a,227);
    assert(bob.receive(frame,plain,227,len)==SessionResult::Replay&&!len);untouched(plain,227);++cases;
  }
  poison(retry);const unsigned calls=na.calls;
  assert(alice.send(2,data,1,retry)==SessionResult::NoSlot&&na.calls==calls);untouched(&retry,sizeof(retry));
  assert(alice.retire(1));assert(alice.send(2,data,1,retry)==SessionResult::Ok);
  poison(retry);
  assert(alice.send(1,data,2,retry)==SessionResult::NonceReused);untouched(&retry,sizeof(retry));
  assert(alice.retry(1,retry)==SessionResult::NotPending);
  ka.enabled=false;assert(!alice.ready());
  assert(alice.retry(2,retry)==SessionResult::NotReady);untouched(&retry,sizeof(retry));
  ka.enabled=true;++ka.ref.generation;assert(!alice.ready());
  assert(alice.retry(2,retry)==SessionResult::NotReady);--ka.ref.generation;
  alice.close();assert(!alice.open(ka.ref,Alice,A,Bob,B));
  assert(alice.send(900,data,1,retry)==SessionResult::NotReady);
  // A new object is still governed by the external lifetime reservation authority.
  PkiSession reopened(crypto,ka,na);assert(reopened.open(ka.ref,Alice,A,Bob,B));
  assert(reopened.send(2,data,1,retry)==SessionResult::NonceUnavailable);untouched(&retry,sizeof(retry));
  assert(reopened.send(2,data,1,retry)==SessionResult::NonceReused);
  ++ka.ref.generation; // Same scalar reimport/rotation must not reset nonce domain.
  PkiSession sameKeyNewGeneration(crypto,ka,na);
  assert(sameKeyNewGeneration.open(ka.ref,Alice,A,Bob,B));
  assert(sameKeyNewGeneration.send(2,data,1,retry)==SessionResult::NonceUnavailable);
  ++cases;

  Keys k2(a);Nonces n2;PkiSession bounded(crypto,k2,n2);assert(bounded.open(k2.ref,Alice,A,Bob,B));
  for(uint32_t id=1000;id<1032;++id){assert(bounded.send(id,data,1,frame)==SessionResult::Ok);assert(bounded.retire(id));}
  poison(frame);
  assert(bounded.send(1032,data,1,frame)==SessionResult::NonceExhausted);untouched(&frame,sizeof(frame));
  assert(bounded.send(1000,data,1,frame)==SessionResult::NonceReused);++cases;

  Keys receiver(b);Nonces nr;PkiSession rx(crypto,receiver,nr);assert(rx.open(receiver.ref,Bob,B,Alice,A));
  for(uint32_t id=2000;id<2032;++id) {
    frame={};frame.from=Alice;frame.to=Bob;frame.id=id;
    assert(crypto.encrypt(a,B,Alice,id,0x10203456,data,227,frame.bytes,239,frame.size));
    assert(rx.receive(frame,plain,227,len)==SessionResult::Ok&&len==227);
  }
  frame.id=2032;assert(crypto.encrypt(a,B,Alice,frame.id,0x33445566,data,227,frame.bytes,239,frame.size));
  memset(plain,0x5a,227);const unsigned reads=receiver.reads;
  assert(rx.receive(frame,plain,227,len)==SessionResult::RxExhausted&&!len&&reads==receiver.reads);untouched(plain,227);++cases;

  Keys receiver2(b);Nonces nr2;PkiSession rx2(crypto,receiver2,nr2);assert(rx2.open(receiver2.ref,Bob,B,Alice,A));
  for(size_t i=0;i<frame.size;++i) {
    auto changed=frame;changed.bytes[i]^=1;memset(plain,0x5a,227);
    assert(rx2.receive(changed,plain,227,len)==SessionResult::CryptoFailure&&!len);untouched(plain,227);
  }
  assert(rx2.receive(frame,plain,227,len)==SessionResult::Ok&&len==227);++cases;
  for(unsigned change=0;change<4;++change) {
    auto changed=frame;
    if(change==0)++changed.from;
    if(change==1)++changed.to;
    if(change==2)changed.size=240;
    if(change==3)changed.id=0;
    const auto result=change<2?SessionResult::PeerMismatch:SessionResult::InvalidArgument;
    assert(rx2.receive(changed,plain,227,len)==result&&!len);
  }

  for(unsigned fault=0;fault<4;++fault) {
    Keys keys(a);Nonces nonces;FailBackend failure(backend);PkiCrypto c(failure);PkiSession s(c,keys,nonces);
    assert(s.open(keys.ref,Alice,A,Bob,B));
    if(fault==0)nonces.fail=true;
    if(fault==1)keys.fail=true;
    if(fault==2)keys.wrong=true;
    if(fault==3)failure.fail=true;
    poison(frame);const unsigned wipes=pkiWipe32;
    const auto wanted=fault==0?SessionResult::NonceUnavailable:fault==3?SessionResult::CryptoFailure:SessionResult::KeyUnavailable;
    assert(s.send(123,data,227,frame)==wanted);untouched(&frame,sizeof(frame));
    if(fault>0)assert(pkiWipe32>wipes);
    assert(s.send(123,data,1,frame)==SessionResult::NonceReused);
    assert(s.retry(123,frame)==SessionResult::NotPending);++cases;
  }
  for(uint32_t bad:{0u,1u,0xfffffffeu,0xffffffffu}) {
    Keys keys(a);Nonces nonce;PkiSession session(crypto,keys,nonce);
    assert(!session.open(keys.ref,bad,A,Bob,B));assert(!session.open(keys.ref,Alice,A,bad,B));
  }
  Keys wrong(a);wrong.wrong=true;Nonces nonce;PkiSession session(crypto,wrong,nonce);
  assert(!session.open(wrong.ref,Alice,A,Bob,B));assert(!session.ready());
  assert(!session.open(wrong.ref,Alice,A,Bob,B));
  // Observe actual Monocypher wipes at the explicit component cleanup boundaries.
  assert(pkiWipeCalls>0 && pkiWipe32>0);
  std::cout<<"PASS "<<cases<<" session groups: two-peer crypto, exact retries, revocation, lifetime reservations, failure burns, bounded replay and wipes\n";
  std::cout<<"INFO host sizeof(PkiSession)="<<sizeof(PkiSession)<<"; no global session or key instantiated in target\n";
}
