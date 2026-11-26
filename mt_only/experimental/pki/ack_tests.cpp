// SPDX-License-Identifier: GPL-3.0-only
#include "PkiAck.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <vector>
using namespace mt::experimental;
using Bytes=std::vector<uint8_t>;
extern bool ackProofExtract(const uint8_t *,size_t,uint8_t *,size_t *,size_t *);
extern "C" bool stockAckAttach(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t,uint8_t *,size_t *);
extern "C" int stockAckVerify(const uint8_t *,const uint8_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,size_t);
namespace {
struct CountBackend:PkiBackend {
  PkiBackend &base;unsigned calls=0;bool fail=false;
  explicit CountBackend(PkiBackend &b):base(b){}
  bool sha256(const uint8_t *p,size_t n,uint8_t out[32])override{++calls;return !fail&&base.sha256(p,n,out);}
  bool hmac256(const uint8_t key[32],const uint8_t *p,size_t n,uint8_t out[32])override{++calls;return !fail&&base.hmac256(key,p,n,out);}
  bool ccm(bool e,const uint8_t key[32],const uint8_t nonce[13],const uint8_t *p,size_t n,uint8_t *out,uint8_t tag[8])override{return base.ccm(e,key,nonce,p,n,out,tag);}
};
void varint(Bytes &out,unsigned n){while(n>127){out.push_back(uint8_t(n|128));n>>=7;}out.push_back(uint8_t(n));}
void unchanged(const uint8_t *p,size_t n){assert(std::all_of(p,p+n,[](uint8_t b){return b==0x5a;}));}
template<class T>void poison(T &value){auto *p=reinterpret_cast<uint8_t *>(&value);std::fill(p,p+sizeof(value),uint8_t(0x5a));}
}
void runAckTests(PkiBackend &backend,const uint8_t *a,const uint8_t *b,const uint8_t *A,const uint8_t *B) {
  CountBackend count(backend);PkiCrypto crypto(count);PkiAck candidate(crypto);
  const AckEnvelope context{true,0x24681234,0x98765432,0x10203040};
  const PendingAck pending{true,context.from,context.to,context.request};
  uint8_t wire[233],stock[233],out[233];size_t len=0,expectedLen=0;
  unsigned vectors=0;
  for(unsigned n=0;n<=218;++n) {
    Bytes routing{0x18,0x00,0x2a};varint(routing,n);
    for(unsigned j=0;j<n;++j)routing.push_back(uint8_t(13*j+n));
    assert(routing.size()<=223);
    assert(candidate.attach(a,B,context,routing.data(),routing.size(),wire,233,len));
    assert(stockAckAttach(a,B,context.from,context.to,context.request,routing.data(),routing.size(),stock,&expectedLen));
    assert(len==expectedLen && !memcmp(wire,stock,len));
    assert(stockAckVerify(b,A,context.from,context.to,context.request,wire,len)==1);
    const size_t wireLen=len;
    assert(candidate.verify(b,A,pending,context,stock,wireLen,out,233,len)==AckProofVerdict::ValidProof);
    assert(len==routing.size() && !memcmp(out,routing.data(),len));
    ProofField field{};uint8_t proof[8];size_t start=0,fieldLen=0;
    assert(PkiAck::inspect(wire,wireLen,field)==ProofFieldResult::Present);
    assert(ackProofExtract(wire,wireLen,proof,&start,&fieldLen));
    assert(field.start==start && field.length==fieldLen && !memcmp(field.proof,proof,8));
    ++vectors;
  }
  // Unknown bytes/field order/nonminimal value and tag encodings survive exactly.
  const Bytes routing{0x2a,3,0x80,0,0xff,0x18,0x80,0x00,0x35,1,2,3,4,
                      0x39,1,2,3,4,5,6,7,8,0x80,0x01,0x81,0};
  assert(candidate.attach(a,B,context,routing.data(),routing.size(),wire,233,len));
  const Bytes proof(wire+routing.size(),wire+len);
  for(size_t offset:{0u,5u,8u,13u,22u,26u}) {
    Bytes moved=routing;moved.insert(moved.begin()+offset,proof.begin(),proof.end());
    assert(stockAckVerify(b,A,context.from,context.to,context.request,moved.data(),moved.size())==1);
    assert(candidate.verify(b,A,pending,context,moved.data(),moved.size(),out,233,len)==AckProofVerdict::ValidProof);
    assert(len==routing.size() && !memcmp(out,routing.data(),len));++vectors;
  }
  // Stock accepts nonminimal encodings of the proof tag/length too; the entire
  // encoded proof field is excised, while all other input bytes stay untouched.
  Bytes nonminimal=routing;
  nonminimal.insert(nonminimal.end(),{0xa2,0x00,0x88,0x00});
  nonminimal.insert(nonminimal.end(),proof.begin()+2,proof.end());
  assert(stockAckVerify(b,A,context.from,context.to,context.request,nonminimal.data(),nonminimal.size())==1);
  assert(candidate.verify(b,A,pending,context,nonminimal.data(),nonminimal.size(),out,233,len)==AckProofVerdict::ValidProof);
  assert(len==routing.size() && !memcmp(out,routing.data(),len));++vectors;
  // A valid proof over a NAK is still valid: caller must interpret Routing.
  const uint8_t nak[]{0x18,9};
  assert(candidate.attach(a,B,context,nak,sizeof(nak),wire,233,len));
  assert(stockAckVerify(b,A,context.from,context.to,context.request,wire,len)==1);
  assert(candidate.verify(b,A,pending,context,wire,len,out,233,expectedLen)==AckProofVerdict::ValidProof);
  assert(expectedLen==sizeof(nak) && !memcmp(out,nak,sizeof(nak)));++vectors;
  std::cout<<"PASS "<<vectors<<" exact stock ACK wire attach/excise/verify vectors\n";

  assert(candidate.attach(a,B,context,routing.data(),routing.size(),wire,233,len));
  Bytes valid(wire,wire+len);
  unsigned mutations=0;
  for(size_t i=0;i<valid.size();++i)for(uint8_t bit:{uint8_t(1),uint8_t(128)}) {
    Bytes changed=valid;changed[i]^=bit;memset(out,0x5a,sizeof(out));len=99;
    assert(candidate.verify(b,A,pending,context,changed.data(),changed.size(),out,233,len)!=AckProofVerdict::ValidProof);
    assert(len==0);unchanged(out,233);++mutations;
  }
  for(unsigned field=0;field<4;++field) {
    auto changed=context;
    if(field==0)changed.from^=1;
    if(field==1)changed.to^=1;
    if(field==2)changed.request^=1;
    if(field==3)changed.decodedRouting=false;
    count.calls=0;
    assert(candidate.verify(b,A,pending,changed,valid.data(),valid.size(),out,233,len)==AckProofVerdict::ContextMismatch);
    assert(count.calls==0);
  }
  for(unsigned invalid=0;invalid<5;++invalid) {
    auto bad=context;
    if(invalid==0)bad.from=0;
    if(invalid==1)bad.to=UINT32_MAX;
    if(invalid==2)bad.to=bad.from;
    if(invalid==3)bad.request=0;
    if(invalid==4)bad.decodedRouting=false;
    memset(out,0x5a,sizeof(out));count.calls=0;
    assert(!candidate.attach(a,B,bad,routing.data(),routing.size(),out,233,len));
    assert(!len && !count.calls);unchanged(out,233);
    assert(candidate.verify(b,A,pending,bad,valid.data(),valid.size(),out,233,len)==AckProofVerdict::ContextMismatch);
    assert(!len && !count.calls);unchanged(out,233);
  }
  auto expired=pending;expired.active=false;count.calls=0;
  assert(candidate.verify(b,A,expired,context,valid.data(),valid.size(),out,233,len)==AckProofVerdict::ContextMismatch);
  assert(count.calls==0);
  assert(candidate.verify(b,nullptr,pending,context,valid.data(),valid.size(),out,233,len)==AckProofVerdict::NoTrustedKey);
  assert(count.calls==0);
  assert(candidate.verify(a,A,pending,context,valid.data(),valid.size(),out,233,len)==AckProofVerdict::InvalidProof);
  // Even if caller incorrectly replaces pending to match the spoofed envelope,
  // direction/request are still covered by the MAC. Peer-key provenance remains
  // outside this API and cannot be established by a proof of the same key itself.
  for(unsigned field=0;field<3;++field) {
    auto changed=context;auto changedPending=pending;
    if(field==0){changed.from^=1;changedPending.peer=changed.from;}
    if(field==1){changed.to^=1;changedPending.local=changed.to;}
    if(field==2){changed.request^=1;changedPending.request=changed.request;}
    assert(candidate.verify(b,A,changedPending,changed,valid.data(),valid.size(),out,233,len)==AckProofVerdict::InvalidProof);
    assert(stockAckVerify(b,A,changed.from,changed.to,changed.request,valid.data(),valid.size())==2);
  }
  std::cout<<"PASS "<<mutations<<" wire mutations; pending, direction, request and key admission\n";

  std::vector<Bytes> malformed{{0x18},{0x22,7,1,2,3,4,5,6,7},{0x22,8,1,2,3,4,5,6,7},
                             {0x20,0},{0,0},{0x2f},{0x80},{0x2a,0xff,0xff,0xff,0xff,0x7f}};
  Bytes duplicate=valid;duplicate.insert(duplicate.end(),proof.begin(),proof.end());malformed.push_back(duplicate);
  Bytes trailing=valid;trailing.push_back(0x18);malformed.push_back(trailing);
  for(const auto &bad:malformed) {
    ProofField field{};poison(field);const ProofField before=field;
    assert(PkiAck::inspect(bad.data(),bad.size(),field)==ProofFieldResult::Malformed);
    assert(field.start==before.start && field.length==before.length && !memcmp(field.proof,before.proof,8));
    memset(out,0x5a,sizeof(out));len=99;count.calls=0;
    assert(candidate.verify(b,A,pending,context,bad.data(),bad.size(),out,233,len)==AckProofVerdict::Malformed);
    assert(!count.calls && !len);unchanged(out,233);
    assert(!candidate.attach(a,B,context,bad.data(),bad.size(),out,233,len));
  }
  // These two are intentionally stricter than the stock extractor, which skips
  // reserved tag zero and a wrong-wire-type proof field as if they were unknown.
  for(const Bytes &suffix:{Bytes{0x20,0},Bytes{0,0}}) {
    Bytes bad=valid;bad.insert(bad.end(),suffix.begin(),suffix.end());
    ProofField field{};uint8_t p[8];size_t pos=0,n=0;
    assert(ackProofExtract(bad.data(),bad.size(),p,&pos,&n));
    assert(PkiAck::inspect(bad.data(),bad.size(),field)==ProofFieldResult::Malformed);
  }
  ProofField field{};
  assert(PkiAck::inspect(nullptr,0,field)==ProofFieldResult::Absent);
  assert(PkiAck::inspect(nullptr,1,field)==ProofFieldResult::Malformed);
  assert(PkiAck::inspect(wire,234,field)==ProofFieldResult::Malformed);
  assert(candidate.verify(b,A,pending,context,routing.data(),routing.size(),out,233,len)==AckProofVerdict::Absent);
  assert(candidate.verify(b,A,pending,context,valid.data(),valid.size(),out,routing.size()-1,len)==AckProofVerdict::InvalidInput);
  assert(!candidate.attach(a,B,context,routing.data(),routing.size(),out,valid.size()-1,len));
  assert(!candidate.attach(a,B,context,valid.data(),valid.size(),out,233,len));
  assert(!candidate.attach(a,B,context,out,224,out,233,len));
  assert(!candidate.attach(a,nullptr,context,routing.data(),routing.size(),out,233,len));
  assert(candidate.attach(a,B,context,nullptr,0,out,233,len)&&len==10);
  assert(candidate.verify(b,A,pending,context,out,len,wire,0,expectedLen)==AckProofVerdict::ValidProof && expectedLen==0);
  // Malformed data is rejected before X25519. Deterministic parser corpus never
  // provisions keys and verifies every candidate acceptance against stock.
  uint32_t seed=0x10203040;unsigned accepted=0;
  for(unsigned j=0;j<20000;++j) {
    Bytes data=valid;
    seed=seed*1664525u+1013904223u;const unsigned edits=1+(seed%3);
    for(unsigned k=0;k<edits;++k){seed=seed*1664525u+1013904223u;const size_t at=seed%data.size();seed=seed*1664525u+1013904223u;data[at]=uint8_t(seed>>16);}
    ProofField f{};const auto result=PkiAck::inspect(data.data(),data.size(),f);
    if(result==ProofFieldResult::Present) {
      uint8_t p[8];size_t pos=0,n=0;
      assert(ackProofExtract(data.data(),data.size(),p,&pos,&n));
      assert(pos==f.start && n==f.length && !memcmp(p,f.proof,8));++accepted;
    }
  }
  count.fail=true;memset(out,0x5a,sizeof(out));
  assert(!candidate.attach(a,B,context,routing.data(),routing.size(),out,233,len)&&!len);unchanged(out,233);
  assert(candidate.verify(b,A,pending,context,valid.data(),valid.size(),out,233,len)==AckProofVerdict::InvalidProof);
  assert(!len);unchanged(out,233);
  std::cout<<"PASS malformed/duplicate/bounds/output containment and 20000 parser cases ("<<accepted<<" stock-matched acceptances)\n";
}
