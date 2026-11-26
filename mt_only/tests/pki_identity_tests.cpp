// SPDX-License-Identifier: GPL-3.0-only
// Synthetic host fixtures only; never linked to a firmware image or real NVS.
#include "pki/PkiIdentity.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

using namespace mt::pki;
using Raw=std::vector<uint8_t>;
static unsigned checks=0,cutPoints=0,corruptBits=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();}}while(0)
static const char *keys[]={"mtidentity/state0","mtidentity/state1"};
static uint32_t crc(const uint8_t *p,size_t n){
  uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1))&0xedb88320u);}return ~c;
}
static void put32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=uint8_t(x>>(8*i));}
static void seal(Raw &r){put32(r.data()+80,crc(r.data(),80));}
struct Fixture {
  std::array<uint8_t,32> scalar{};
  IdentityBinding binding;
  explicit Fixture(unsigned seed,uint32_t generation=1){
    for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+17*i);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;
    crypto_x25519_public_key(binding.publicKey,scalar.data());
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=generation;
  }
  IdentityCandidate candidate()const{return IdentityCandidate(binding,scalar.data());}
};
struct Memory final:mt::SettingsBackend {
  std::map<std::string,Raw> data;
  unsigned reads=0,writes=0;
  int failRead=0,writeResult=0,cut=-1,readLength=-999;
  bool commit=true,mutateReadback=false;
  int read(const char *key,uint8_t *out,size_t capacity) override {
    ++reads;CHECK(std::string(key)==keys[0]||std::string(key)==keys[1]);
    if(failRead)return failRead;
    if(readLength!=-999&&writes)return readLength;
    auto it=data.find(key);if(it==data.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    if(!it->second.empty())memcpy(out,it->second.data(),it->second.size());
    if(mutateReadback&&writes&&it->second.size())out[0]^=1;
    return int(it->second.size());
  }
  int write(const char *key,const uint8_t *raw,size_t n) override {
    ++writes;CHECK(std::string(key)==keys[0]||std::string(key)==keys[1]);CHECK(n==IdentityJournal::RecordSize);
    if(commit){const size_t count=cut<0?n:size_t(cut);CHECK(count<=n);data[key]=Raw(raw,raw+count);}
    return writeResult;
  }
};
struct Observe final:PrivateKeyOperation {
  const uint8_t *expected;unsigned calls=0;bool result=true;
  explicit Observe(const uint8_t *p):expected(p){}
  bool apply(const uint8_t scalar[32]) override {++calls;CHECK(!memcmp(scalar,expected,32));return result;}
};
static bool contains(const void *object,size_t n,const uint8_t *secret){
  const auto *p=static_cast<const uint8_t *>(object);for(size_t i=0;i+32<=n;++i)if(!memcmp(p+i,secret,32))return true;return false;
}
static KeyRef metadata(IdentityJournal &j,const Fixture &f){
  IdentityBinding b;KeyRef ref;CHECK(j.binding(b,ref));CHECK(b.version==1&&b.algorithm==1);
  CHECK(b.node==f.binding.node&&b.generation==f.binding.generation&&!memcmp(b.publicKey,f.binding.publicKey,32));
  CHECK(j.active(ref));Observe op(f.scalar.data());CHECK(j.withPrivate(ref,op)&&op.calls==1);return ref;
}
static void disabled(IdentityJournal &j,KeyRef ref,const Fixture &f){
  CHECK(!j.active(ref));IdentityBinding b;b.node=123;KeyRef out{8,9};CHECK(!j.binding(b,out));
  CHECK(b.node==123&&out.slot==8&&out.generation==9);Observe op(f.scalar.data());CHECK(!j.withPrivate(ref,op)&&!op.calls);
  CHECK(!contains(&j,sizeof(j),f.scalar.data()));
}
static Memory stored(const Fixture &f,MonocypherIdentityDeriver &d){
  Memory m;IdentityJournal j;CHECK(!j.load(m,d));CHECK(!j.store(m,d,f.candidate(),true));metadata(j,f);return m;
}

int main(){
  static_assert(!std::is_copy_constructible<IdentityCandidate>::value);
  static_assert(!std::is_copy_constructible<IdentityJournal>::value);
  static_assert(!std::is_move_constructible<IdentityJournal>::value);
  MonocypherIdentityDeriver derive;Fixture first(11),next(59,2),third(107,3);
  {
    Memory m;IdentityJournal j;disabled(j,{0,0},first);CHECK(j.store(m,derive,first.candidate(),true)==-EACCES);
    CHECK(!m.reads&&!m.writes);CHECK(!j.load(m,derive));CHECK(j.state()==IdentityJournal::State::Empty);
    CHECK(m.reads==2&&!m.writes);disabled(j,{0,1},first);
    CHECK(j.load(m,derive)==-EACCES);CHECK(j.store(m,derive,first.candidate(),false)==-EAGAIN);CHECK(!m.writes);
    CHECK(!j.store(m,derive,first.candidate(),true));auto old=metadata(j,first);CHECK(old.slot==0&&m.writes==1);
    CHECK(j.store(m,derive,first.candidate(),true)==-ESTALE);CHECK(m.writes==1);metadata(j,first);
    Fixture skip(59,3);CHECK(j.store(m,derive,skip.candidate(),true)==-ESTALE);metadata(j,first);
    const Raw previous=m.data[keys[0]];CHECK(!j.store(m,derive,next.candidate(),true));auto newer=metadata(j,next);
    CHECK(newer.slot==1&&!j.active(old));CHECK(m.data[keys[0]]==previous);
    Observe stale(first.scalar.data());CHECK(!j.withPrivate(old,stale)&&!stale.calls);
    const Raw second=m.data[keys[1]];CHECK(!j.store(m,derive,third.candidate(),true));metadata(j,third);
    CHECK(m.data[keys[1]]==second);IdentityJournal reloaded;CHECK(!reloaded.load(m,derive));metadata(reloaded,third);
    Fixture same=third;same.binding.generation=4;const auto prior=metadata(j,third);
    CHECK(!j.store(m,derive,same.candidate(),true));CHECK(!j.active(prior));metadata(j,same); // no no-op elision
  }
  {
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));const auto ref=metadata(j,first);const auto previous=m.data;
    for(unsigned field=0;field<7;++field){
      Fixture bad=next;
      if(field==0)bad.binding.version=2;
      if(field==1)bad.binding.algorithm=2;
      if(field==2)bad.scalar[0]|=1;
      if(field==3)bad.scalar[31]|=128;
      if(field==4)bad.scalar[31]&=63;
      if(field==5)bad.binding.node^=8;
      if(field==6){bad.binding.publicKey[4]^=1;bad.binding.node=bench::MtSignature::nodeId(bad.binding.publicKey);}
      CHECK(j.store(m,derive,bad.candidate(),true)==-EINVAL);CHECK(m.data==previous&&m.writes==1);CHECK(j.active(ref));metadata(j,first);
    }
    struct Failure:IdentityDeriver {bool derivePublic(const uint8_t *,uint8_t *out)override{memset(out,0xa5,32);return false;}} fail;
    CHECK(j.store(m,fail,next.candidate(),true)==-EINVAL);CHECK(m.data==previous);metadata(j,first);
    IdentityCandidate null(next.binding,nullptr);CHECK(j.store(m,derive,null,true)==-EINVAL);metadata(j,first);
  }
  // Every possible byte interruption during first provisioning, first rotation,
  // and overwriting an older inactive slot. No memory provider survives uncertainty.
  for(unsigned path=0;path<3;++path){
    Memory initial;if(path)initial=stored(first,derive);
    if(path==2){IdentityJournal j;CHECK(!j.load(initial,derive));CHECK(!j.store(initial,derive,next.candidate(),true));}
    for(unsigned cut=0;cut<=IdentityJournal::RecordSize;++cut){
      ++cutPoints;Memory m=initial;IdentityJournal j;CHECK(!j.load(m,derive));
      const Fixture &candidate=path==0?first:path==1?next:third;
      const Fixture &old=path==2?next:first;
      KeyRef before{0,0};if(path)before=metadata(j,old);
      const unsigned active=path==2?1:0;const Raw preserved=path?m.data[keys[active]]:Raw{};
      m.cut=int(cut);m.writeResult=-EIO;
      CHECK(j.store(m,derive,candidate.candidate(),true)==-EIO);CHECK(j.state()==IdentityJournal::State::IoFrozen);disabled(j,before,old);
      CHECK(j.store(m,derive,candidate.candidate(),true)==-EACCES);CHECK(j.load(m,derive)==-EACCES);
      if(path)CHECK(m.data[keys[active]]==preserved);
      IdentityJournal restart;CHECK(restart.load(m,derive)==(cut==IdentityJournal::RecordSize?0:-EILSEQ));
      if(cut==IdentityJournal::RecordSize)metadata(restart,candidate);
      else CHECK(restart.state()==IdentityJournal::State::CorruptFrozen);
    }
  }
  {
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));const auto ref=metadata(j,first);const auto previous=m.data;
    m.commit=false;m.writeResult=-EIO;CHECK(j.store(m,derive,next.candidate(),true)==-EIO);disabled(j,ref,first);CHECK(m.data==previous);
    IdentityJournal restart;CHECK(!restart.load(m,derive));metadata(restart,first);
  }
  {
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));const auto ref=metadata(j,first);
    m.writeResult=1;CHECK(j.store(m,derive,next.candidate(),true)==-EIO);disabled(j,ref,first); // bad positive backend status
  }
  for(int readStatus:{-EIO,-ENOENT,-E2BIG,0,83,85}){
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));const auto ref=metadata(j,first);
    m.readLength=readStatus;CHECK(j.store(m,derive,next.candidate(),true)<0);disabled(j,ref,first);
    m.readLength=-999;IdentityJournal restart;CHECK(!restart.load(m,derive));metadata(restart,next);
  }
  {
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));const auto ref=metadata(j,first);m.mutateReadback=true;
    CHECK(j.store(m,derive,next.candidate(),true)==-EIO);disabled(j,ref,first);
  }
  {
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));const auto ref=metadata(j,first);m.commit=false;
    CHECK(j.store(m,derive,next.candidate(),true)==-ENOENT);disabled(j,ref,first);
  }
  Memory two=stored(first,derive);{IdentityJournal j;CHECK(!j.load(two,derive));CHECK(!j.store(two,derive,next.candidate(),true));}
  for(unsigned slot=0;slot<2;++slot)for(unsigned bit=0;bit<IdentityJournal::RecordSize*8;++bit){
    ++corruptBits;Memory m=two;m.data[keys[slot]][bit/8]^=uint8_t(1u<<(bit%8));IdentityJournal j;
    CHECK(j.load(m,derive)==-EILSEQ);CHECK(j.state()==IdentityJournal::State::CorruptFrozen);disabled(j,{slot,slot+1},slot?next:first);
    CHECK(j.store(m,derive,third.candidate(),true)==-EACCES);CHECK(j.load(m,derive)==-EACCES);
  }
  for(unsigned mutation=0;mutation<11;++mutation){
    Memory m=two;Raw &r=m.data[keys[1]];
    switch(mutation){
      case 0:r[4]=2;break;case 1:r[5]=2;break;case 2:r[6]=0;break;case 3:r[7]=1;break;
      case 4:put32(r.data()+8,0);break;case 5:put32(r.data()+12,1);break;
      case 6:r[16]|=1;break;case 7:r[47]|=128;break;case 8:r[47]&=63;break;
      case 9:r[49]^=1;put32(r.data()+12,bench::MtSignature::nodeId(r.data()+48));break;
      case 10:put32(r.data()+12,0xfffffffeu);break;
    }
    seal(r);IdentityJournal j;CHECK(j.load(m,derive)==-EILSEQ);CHECK(j.state()==IdentityJournal::State::CorruptFrozen);
  }
  for(uint32_t reserved:{0u,1u,0xfffffffeu,0xffffffffu}){
    Memory m=two;Raw &r=m.data[keys[1]];put32(r.data()+12,reserved);seal(r);
    IdentityJournal j;CHECK(j.load(m,derive)==-EILSEQ);
  }
  {
    Memory m=two;put32(m.data[keys[1]].data()+8,1);seal(m.data[keys[1]]);IdentityJournal j;CHECK(j.load(m,derive)==-EILSEQ);
    m.data[keys[1]]=m.data[keys[0]];IdentityJournal same;CHECK(!same.load(m,derive));metadata(same,first);
  }
  for(uint32_t generation:{3u,0x80000001u,UINT32_MAX}){
    Memory m=two;put32(m.data[keys[1]].data()+8,generation);seal(m.data[keys[1]]);IdentityJournal j;CHECK(j.load(m,derive)==-EILSEQ);
  }
  {
    Memory m=two;m.data.erase(keys[0]);IdentityJournal j;CHECK(j.load(m,derive)==-EILSEQ); // missing older slot after rotation
    m=two;std::swap(m.data[keys[0]],m.data[keys[1]]);IdentityJournal reversed;CHECK(!reversed.load(m,derive));metadata(reversed,next);
  }
  {
    Memory m=two;put32(m.data[keys[0]].data()+8,UINT32_MAX-1);seal(m.data[keys[0]]);
    put32(m.data[keys[1]].data()+8,UINT32_MAX);seal(m.data[keys[1]]);IdentityJournal j;CHECK(!j.load(m,derive));
    Fixture last=next;last.binding.generation=UINT32_MAX;const auto ref=metadata(j,last);
    CHECK(j.store(m,derive,first.candidate(),true)==-EOVERFLOW);CHECK(j.active(ref));
  }
  for(int error:{-EIO,-ENOMEM,-EAGAIN}){
    Memory m=two;m.failRead=error;IdentityJournal j;CHECK(j.load(m,derive)==error);CHECK(j.state()==IdentityJournal::State::IoFrozen);
    disabled(j,{0,1},first);CHECK(j.load(m,derive)==-EACCES);
  }
  for(unsigned n:{0u,83u,85u}){
    Memory m=two;m.data[keys[0]].resize(n);IdentityJournal j;CHECK(j.load(m,derive)==-EILSEQ);
  }
  // Reentrant trusted callbacks cannot invalidate a borrowed scalar or start a
  // second operation. They cannot obtain metadata while mutation is in progress.
  {
    Memory m=stored(first,derive);IdentityJournal j;CHECK(!j.load(m,derive));auto ref=metadata(j,first);
    struct Reenter final:PrivateKeyOperation {
      IdentityJournal &j;Memory &m;MonocypherIdentityDeriver &d;Fixture &f;KeyRef ref;
      Reenter(IdentityJournal &a,Memory &b,MonocypherIdentityDeriver &c,Fixture &e,KeyRef r):j(a),m(b),d(c),f(e),ref(r){}
      bool apply(const uint8_t scalar[32])override{
        CHECK(!memcmp(scalar,f.scalar.data(),32));CHECK(!j.active(ref));CHECK(!j.close());
        CHECK(j.load(m,d)==-EBUSY);CHECK(j.store(m,d,f.candidate(),true)==-EBUSY);
        Observe inner(f.scalar.data());CHECK(!j.withPrivate(ref,inner)&&!inner.calls);IdentityBinding b;KeyRef r;CHECK(!j.binding(b,r));return true;
      }
    } reenter(j,m,derive,first,ref);
    CHECK(j.withPrivate(ref,reenter));metadata(j,first);Observe falseOp(first.scalar.data());falseOp.result=false;
    CHECK(!j.withPrivate(ref,falseOp));CHECK(j.active(ref));CHECK(j.close());disabled(j,ref,first);CHECK(j.load(m,derive)==-EACCES);
    CHECK(j.store(m,derive,next.candidate(),true)==-EACCES);CHECK(j.close());
  }
  // Explicit move/destruction clears the owned scalar; no fixture bytes are in
  // production sources. Reading unsigned-byte backing storage checks erasure.
  {
    alignas(IdentityCandidate) uint8_t memory[sizeof(IdentityCandidate)];
    auto *a=new(memory)IdentityCandidate(first.binding,first.scalar.data());CHECK(contains(memory,sizeof(memory),first.scalar.data()));
    IdentityCandidate moved(static_cast<IdentityCandidate&&>(*a));CHECK(!contains(memory,sizeof(memory),first.scalar.data()));a->~IdentityCandidate();
    auto *b=new(memory)IdentityCandidate(first.binding,first.scalar.data());b->~IdentityCandidate();CHECK(!contains(memory,sizeof(memory),first.scalar.data()));
    Memory m;IdentityJournal j;CHECK(!j.load(m,derive));CHECK(!j.store(m,derive,moved,true));metadata(j,first);
    alignas(IdentityJournal) uint8_t storage[sizeof(IdentityJournal)];auto *p=new(storage)IdentityJournal;
    CHECK(!p->load(m,derive));CHECK(contains(storage,sizeof(storage),first.scalar.data()));p->~IdentityJournal();CHECK(!contains(storage,sizeof(storage),first.scalar.data()));
    uint8_t scratch[32];memset(scratch,0xa5,sizeof(scratch));eraseIdentityBytes(scratch,sizeof(scratch));for(auto byte:scratch)CHECK(!byte);
  }
  CHECK(!derive.derivePublic(nullptr,nullptr));
  std::printf("{\"PASS\":true,\"assertions\":%u,\"interruption_cut_points\":%u,\"corrupt_single_bits\":%u,\"record_size\":%zu}\n",checks,cutPoints,corruptBits,IdentityJournal::RecordSize);
}
