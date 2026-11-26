// SPDX-License-Identifier: GPL-3.0-only
#include "PkiIdentity.h"
#include <limits.h>
#include <string.h>
#include "MtSignature.h"
#include "monocypher.h"

namespace mt::pki {
void eraseIdentityBytes(void *data,size_t length){
  volatile uint8_t *p=static_cast<volatile uint8_t *>(data);
  while(length--)*p++=0;
}
namespace {
template<size_t N> struct Scratch {
  uint8_t bytes[N]{};
  Scratch()=default;
  Scratch(const Scratch &)=delete;
  Scratch &operator=(const Scratch &)=delete;
  ~Scratch(){eraseIdentityBytes(bytes,N);}
};
struct Busy {
  bool &flag;
  explicit Busy(bool &f):flag(f){flag=true;}
  ~Busy(){flag=false;}
};
const char *key(unsigned slot){return slot?"mtidentity/state1":"mtidentity/state0";}
uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
uint32_t crc32(const uint8_t *p,size_t n){
  uint32_t c=UINT32_MAX;
  for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
  return ~c;
}
bool equal(const uint8_t *a,const uint8_t *b,size_t n){
  uint8_t diff=0;for(size_t i=0;i<n;++i)diff|=a[i]^b[i];return diff==0;
}
bool valid(const IdentityBinding &b,const uint8_t scalar[32],IdentityDeriver &deriver){
  if(b.version!=1||b.algorithm!=1||!b.generation||b.node<=1||b.node>=0xfffffffeu||
     (scalar[0]&7)||(scalar[31]&0xc0)!=0x40||bench::MtSignature::nodeId(b.publicKey)!=b.node)return false;
  Scratch<32> derived,edwards;
  return deriver.derivePublic(scalar,derived.bytes)&&equal(b.publicKey,derived.bytes,32)&&
         bench::MtSignature::admissiblePublicKey(b.publicKey,edwards.bytes);
}
void encode(const IdentityBinding &b,const uint8_t scalar[32],uint8_t *raw){
  memset(raw,0,IdentityJournal::RecordSize);
  memcpy(raw,"ZTID",4);raw[4]=b.version;raw[5]=b.algorithm;raw[6]=IdentityJournal::RecordSize;
  put32(raw+8,b.generation);put32(raw+12,b.node);
  memcpy(raw+16,scalar,32);memcpy(raw+48,b.publicKey,32);put32(raw+80,crc32(raw,80));
}
bool decode(const uint8_t *raw,int n,IdentityBinding &b,IdentityDeriver &deriver){
  if(n!=int(IdentityJournal::RecordSize)||memcmp(raw,"ZTID",4)||raw[6]!=IdentityJournal::RecordSize||
     raw[7]||get32(raw+80)!=crc32(raw,80))return false;
  b.version=raw[4];b.algorithm=raw[5];b.generation=get32(raw+8);b.node=get32(raw+12);
  memcpy(b.publicKey,raw+48,32);return valid(b,raw+16,deriver);
}
}

bool MonocypherIdentityDeriver::derivePublic(const uint8_t scalar[32],uint8_t output[32]){
  if(!scalar||!output)return false;
  crypto_x25519_public_key(output,scalar);return true;
}
IdentityCandidate::IdentityCandidate(const IdentityBinding &b,const uint8_t scalar[32]):binding_(b){
  if(scalar)memcpy(scalar_,scalar,32);
}
IdentityCandidate::IdentityCandidate(IdentityCandidate &&other) noexcept:binding_(other.binding_){
  memcpy(scalar_,other.scalar_,32);eraseIdentityBytes(other.scalar_,32);other.binding_=IdentityBinding{};
}
IdentityCandidate::~IdentityCandidate(){eraseIdentityBytes(scalar_,32);}
void IdentityJournal::clearActive(){eraseIdentityBytes(scalar_,32);binding_=IdentityBinding{};active_=-1;}
int IdentityJournal::freeze(State s,int error){clearActive();state_=s;return error;}
IdentityJournal::~IdentityJournal(){clearActive();}

int IdentityJournal::load(SettingsBackend &backend,IdentityDeriver &deriver){
  if(busy_)return -EBUSY;
  if(state_!=State::Unloaded)return -EACCES;
  Busy guard(busy_);
  Scratch<RecordSize> raw[2];IdentityBinding bindings[2];bool present[2]{};
  for(unsigned i=0;i<2;++i){
    const int n=backend.read(key(i),raw[i].bytes,RecordSize);
    if(n==-ENOENT)continue;
    if(n<0&&n!=-E2BIG)return freeze(State::IoFrozen,n);
    if(!decode(raw[i].bytes,n,bindings[i],deriver))return freeze(State::CorruptFrozen,-EILSEQ);
    present[i]=true;
  }
  if(!present[0]&&!present[1]){state_=State::Empty;return 0;}
  unsigned slot=present[1]?1:0;
  if(present[0]&&present[1]){
    const uint32_t a=bindings[0].generation,b=bindings[1].generation;
    if(a==b){
      if(!equal(raw[0].bytes,raw[1].bytes,RecordSize))return freeze(State::CorruptFrozen,-EILSEQ);
      slot=0;
    }else{
      const uint32_t lo=a<b?a:b,hi=a<b?b:a;
      if(hi-lo!=1)return freeze(State::CorruptFrozen,-EILSEQ);
      slot=a<b?1:0;
    }
  }else if(bindings[slot].generation!=1){
    // A lone later generation cannot result from this journal's normal writes.
    return freeze(State::CorruptFrozen,-EILSEQ);
  }
  binding_=bindings[slot];memcpy(scalar_,raw[slot].bytes+16,32);
  active_=int8_t(slot);state_=State::Ready;return 0;
}

int IdentityJournal::store(SettingsBackend &backend,IdentityDeriver &deriver,const IdentityCandidate &candidate,bool quiescent){
  if(busy_)return -EBUSY;
  if(state_!=State::Empty&&state_!=State::Ready)return -EACCES;
  if(!quiescent)return -EAGAIN;
  if(binding_.generation==UINT32_MAX)return -EOVERFLOW;
  if(candidate.binding_.generation!=binding_.generation+1)return -ESTALE;
  Busy guard(busy_);
  if(!valid(candidate.binding_,candidate.scalar_,deriver))return -EINVAL;
  const unsigned slot=active_==0?1:0;
  Scratch<RecordSize> record,readback;encode(candidate.binding_,candidate.scalar_,record.bytes);
  const int result=backend.write(key(slot),record.bytes,RecordSize);
  if(result)return freeze(State::IoFrozen,result<0?result:-EIO);
  const int n=backend.read(key(slot),readback.bytes,RecordSize);
  if(n!=int(RecordSize)||!equal(record.bytes,readback.bytes,RecordSize))
    return freeze(State::IoFrozen,n<0?n:-EIO);
  // Validation and exact readback succeeded under one serialized ownership scope.
  eraseIdentityBytes(scalar_,32);memcpy(scalar_,candidate.scalar_,32);
  binding_=candidate.binding_;active_=int8_t(slot);state_=State::Ready;return 0;
}
bool IdentityJournal::binding(IdentityBinding &out,KeyRef &ref)const{
  if(busy_||state_!=State::Ready)return false;
  out=binding_;ref={uint32_t(active_),binding_.generation};return true;
}
bool IdentityJournal::withPrivate(KeyRef ref,PrivateKeyOperation &operation){
  if(!active(ref))return false;
  Busy guard(busy_);return operation.apply(scalar_);
}
bool IdentityJournal::active(KeyRef ref)const{
  return !busy_&&state_==State::Ready&&ref.slot==uint32_t(active_)&&ref.generation==binding_.generation;
}
bool IdentityJournal::close(){
  if(busy_)return false;
  clearActive();state_=State::Closed;return true;
}
}
