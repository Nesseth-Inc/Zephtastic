// SPDX-License-Identifier: GPL-3.0-only
#include "PkiLedger.h"
#include "MtSignature.h"
#include <string.h>

namespace mt::pki {
namespace {
struct Busy {bool &v;explicit Busy(bool &b):v(b){v=true;}~Busy(){v=false;}};
template<size_t N> struct Scratch {uint8_t bytes[N]{};~Scratch(){eraseIdentityBytes(bytes,N);}};
uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
bool node(uint32_t n){return n>1&&n<0xfffffffeu;}
const char *key(unsigned slot){return slot?"mtpki/ledger1":"mtpki/ledger0";}
uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
bool anchorValid(const uint8_t *p,int n,AppendAnchor &out){
  if(n!=int(PkiLedger::RecordSize)||memcmp(p,"ZTPA",4)||p[4]!=3||p[5]||p[6]||p[7]||
     get32(p+PkiLedger::RecordSize-4)!=crc(p,PkiLedger::RecordSize-4))return false;
  for(size_t i=24;i<PkiLedger::RecordSize-4;++i)if(p[i])return false;
  AppendAnchor candidate{get32(p+8),get32(p+12),get32(p+16),get32(p+20)};
  if(!candidate.generation||candidate.capacity!=172032)return false;
  out=candidate;return true;
}
bool publicKey(const uint8_t *p){Scratch<32> ed;return p&&bench::MtSignature::admissiblePublicKey(p,ed.bytes);}
const uint8_t *peer(const uint8_t *r,unsigned i){return r+PkiLedger::PeerOffset+i*PkiLedger::PeerSize;}
uint8_t *peer(uint8_t *r,unsigned i){return r+PkiLedger::PeerOffset+i*PkiLedger::PeerSize;}
bool hasId(const uint8_t *p,unsigned count,uint32_t id){for(unsigned i=0;i<count;++i)if(get32(p+i*4)==id)return true;return false;}
bool idsValid(const uint8_t *p,unsigned count,unsigned limit){
  if(count>limit)return false;
  for(unsigned i=0;i<limit;++i){const uint32_t id=get32(p+i*4);if(i<count){if(!id||hasId(p,i,id))return false;}else if(id)return false;}
  return true;
}
template<unsigned Sends,unsigned Receives,unsigned Version> bool valid(const uint8_t *r,int n){
  constexpr unsigned Peers=4,Header=52,PeerBytes=40+Receives*4,Offset=Header+Sends*4,Size=Offset+Peers*PeerBytes+4;
  if(n!=int(Size)||memcmp(r,"ZTPL",4)||r[4]!=Version||r[5]!=Sends||r[6]!=Peers||r[7]!=Receives||
     r[49]>Peers||r[50]||r[51]||get32(r+Size-4)!=crc(r,Size-4)||
     !node(get32(r+12))||bench::MtSignature::nodeId(r+16)!=get32(r+12)||!publicKey(r+16)||
     !idsValid(r+Header,r[48],Sends))return false;
  uint32_t operations=1+r[48]+r[49];
  for(unsigned i=0;i<Peers;++i){
    const uint8_t *p=r+Offset+i*PeerBytes;
    if(i>=r[49]){for(size_t b=0;b<PeerBytes;++b)if(p[b])return false;continue;}
    if(!node(get32(p))||get32(p)==get32(r+12)||!memcmp(p+4,r+16,32)||!publicKey(p+4)||p[37]>1||p[38]||p[39]||
       !idsValid(p+40,p[36],Receives))return false;
    for(unsigned j=0;j<i;++j){const uint8_t *prior=r+Offset+j*PeerBytes;
      if(get32(prior)==get32(p)||!memcmp(prior+4,p+4,32))return false;}
    operations+=p[36]+p[37];
  }
  return get32(r+8)==operations; // Every permitted mutation appends one item.
}
// Strict wire validation precedes the only legacy conversion. Expand in place
// from the last peer backwards, so no second full-record stack buffer is needed.
// This is RAM normalization, never a boot-time storage write or repair.
bool normalize(uint8_t *r,int n){
  using L=PkiLedger;
  if(valid<L::SendLimit,L::ReceiveLimit,2>(r,n))return true;
  if(!valid<128,64,1>(r,n))return false;
  constexpr size_t OldOffset=52+128*4,OldPeer=40+64*4;
  const unsigned count=r[49];
  for(unsigned i=count;i>0;--i){
    uint8_t *destination=peer(r,i-1);
    memmove(destination,r+OldOffset+(i-1)*OldPeer,OldPeer);
    memset(destination+OldPeer,0,L::PeerSize-OldPeer);
  }
  memset(r+52+128*4,0,(L::SendLimit-128)*4);
  memset(r+L::PeerOffset+count*L::PeerSize,0,(L::PeerLimit-count)*L::PeerSize);
  r[4]=2;r[5]=uint8_t(L::SendLimit);r[7]=uint8_t(L::ReceiveLimit);
  put32(r+L::RecordSize-4,crc(r,L::RecordSize-4));
  return valid<L::SendLimit,L::ReceiveLimit,2>(r,int(L::RecordSize));
}
bool extends(const uint8_t *old,const uint8_t *next){
  using L=PkiLedger;
  if(get32(next+8)!=get32(old+8)+1||memcmp(old+12,next+12,36)||old[48]>next[48]||old[49]>next[49]||
     memcmp(old+L::HeaderSize,next+L::HeaderSize,size_t(old[48])*4))return false;
  for(unsigned i=0;i<old[49];++i){const uint8_t *a=peer(old,i),*b=peer(next,i);
    if(memcmp(a,b,36)||a[36]>b[36]||a[37]>b[37]||(a[37]&&a[36]!=b[36])||
       memcmp(a+40,b+40,size_t(a[36])*4))return false;}
  return true;
}
bool identityMatches(IdentityJournal &identity,const PkiLedger &ledger,KeyRef ref,const uint8_t *pub,uint32_t local){
  IdentityBinding b;KeyRef current;
  return pub&&identity.binding(b,current)&&current.slot==ref.slot&&current.generation==ref.generation&&
         b.node==local&&!memcmp(b.publicKey,pub,32)&&ledger.boundTo(pub,local);
}
}

PkiLedger::~PkiLedger(){eraseIdentityBytes(record_,sizeof(record_));}
int PkiLedger::freeze(State s,int result){eraseIdentityBytes(record_,sizeof(record_));slot_=-1;wireVersion_=0;anchored_=false;anchor_={};state_=s;return result;}
int PkiLedger::load(SettingsBackend &backend){
  if(busy_)return -EBUSY;
  if(state_!=State::Unloaded)return -EACCES;
  Busy guard(busy_);Scratch<RecordSize> raw;bool any=false;unsigned count=0,wireVersion=0,selectors=0;
  for(unsigned i=0;i<2;++i){
    const int n=backend.read(key(i),raw.bytes,RecordSize);if(n==-ENOENT)continue;
    if(n<0&&n!=-E2BIG)return freeze(State::IoFrozen,n);
    if(!memcmp(raw.bytes,"ZTPA",4)) {
      if(++selectors!=1||!anchorValid(raw.bytes,n,anchor_))return freeze(State::CorruptFrozen,-EILSEQ);
      continue;
    }
    const unsigned incomingVersion=raw.bytes[4];
    if(!normalize(raw.bytes,n))return freeze(State::CorruptFrozen,-EILSEQ);
    ++count;
    if(any){
      const uint32_t a=get32(record_+8),b=get32(raw.bytes+8);
      if(a==b){if(wireVersion!=incomingVersion||memcmp(record_,raw.bytes,RecordSize))return freeze(State::CorruptFrozen,-EILSEQ);continue;}
      if(a<b){if(wireVersion>incomingVersion||!extends(record_,raw.bytes))return freeze(State::CorruptFrozen,-EILSEQ);}
      else {if(incomingVersion>wireVersion||!extends(raw.bytes,record_))return freeze(State::CorruptFrozen,-EILSEQ);continue;}
    }
    memcpy(record_,raw.bytes,RecordSize);slot_=int8_t(i);wireVersion=incomingVersion;wireVersion_=uint8_t(wireVersion);any=true;
  }
  // A lone generation >1 cannot arise from a complete history written here.
  if(selectors) {
    if(count!=1||!any||get32(record_+8)!=anchor_.generation||
       get32(record_+RecordSize-4)!=anchor_.snapshotCrc)return freeze(State::CorruptFrozen,-EILSEQ);
    anchored_=true;
  } else if(count==1&&get32(record_+8)!=1)return freeze(State::CorruptFrozen,-EILSEQ);
  state_=any?State::Ready:State::Empty;return 0;
}
bool PkiLedger::snapshotRead(size_t offset,uint8_t *out,size_t size)const{
  if(!ready()||!out||offset>RecordSize||size>RecordSize-offset)return false;
  memcpy(out,record_+offset,size);return true;
}
bool PkiLedger::validateSnapshot(const uint8_t *snapshot,size_t size){
  return snapshot&&size==RecordSize&&valid<SendLimit,ReceiveLimit,2>(snapshot,int(size));
}
bool PkiLedger::matchesAppendSelector(const uint8_t *record,size_t size,const AppendAnchor &expected){
  AppendAnchor actual;
  return record&&size==RecordSize&&anchorValid(record,int(size),actual)&&
    actual.generation==expected.generation&&actual.snapshotCrc==expected.snapshotCrc&&
    actual.rootCrc==expected.rootCrc&&actual.capacity==expected.capacity;
}
int PkiLedger::loadValidatedAppendBaseline(const uint8_t *snapshot,size_t size,
                                           const AppendAnchor &anchor,uint32_t localNode,
                                           const uint8_t localPublic[32]){
  if(state_!=State::Unloaded||busy_)return -EACCES;
  if(!localPublic||!node(localNode)||bench::MtSignature::nodeId(localPublic)!=localNode||
     anchor.capacity!=172032||!validateSnapshot(snapshot,size)||
     get32(snapshot+8)!=anchor.generation||get32(snapshot+RecordSize-4)!=anchor.snapshotCrc||
     get32(snapshot+12)!=localNode||memcmp(snapshot+16,localPublic,32))return -EILSEQ;
  memcpy(record_,snapshot,RecordSize);slot_=-1;wireVersion_=2;anchored_=true;anchor_=anchor;state_=State::Ready;return 0;
}
bool PkiLedger::appendAnchor(AppendAnchor &out)const{
  if(!ready()||!anchored_)return false;
  out=anchor_;return true;
}
int PkiLedger::installAppendAnchor(SettingsBackend &backend,const AppendAnchor &anchor,bool quiescent){
  if(busy_)return -EBUSY;
  if(!ready()||anchored_)return -EACCES;
  if(!quiescent)return -EAGAIN;
  if(anchor.capacity!=172032||anchor.generation!=get32(record_+8)||
     anchor.snapshotCrc!=get32(record_+RecordSize-4))return -EINVAL;
  Busy guard(busy_);Scratch<RecordSize> selector;
  memcpy(selector.bytes,"ZTPA",4);selector.bytes[4]=3;
  put32(selector.bytes+8,anchor.generation);put32(selector.bytes+12,anchor.snapshotCrc);
  put32(selector.bytes+16,anchor.rootCrc);put32(selector.bytes+20,anchor.capacity);
  put32(selector.bytes+RecordSize-4,crc(selector.bytes,RecordSize-4));
  const unsigned next=slot_==0?1:0;
  int result=backend.write(key(next),selector.bytes,RecordSize);
  if(result)return freeze(State::IoFrozen,result<0?result:-EIO);
  const int n=backend.read(key(next),selector.bytes,RecordSize);AppendAnchor verified;
  if(!anchorValid(selector.bytes,n,verified)||verified.generation!=anchor.generation||
     verified.snapshotCrc!=anchor.snapshotCrc||verified.rootCrc!=anchor.rootCrc||
     verified.capacity!=anchor.capacity)return freeze(State::IoFrozen,n<0?n:-EIO);
  anchored_=true;anchor_=anchor;return 0;
}
int PkiLedger::commit(SettingsBackend &backend){
  if(anchored_)return -EACCES;
  const unsigned next=slot_==0?1:0;
  put32(record_+8,get32(record_+8)+1);put32(record_+RecordSize-4,crc(record_,RecordSize-4));
  const int result=backend.write(key(next),record_,RecordSize);
  if(result)return freeze(State::IoFrozen,result<0?result:-EIO);
  Scratch<RecordSize> readback;const int n=backend.read(key(next),readback.bytes,RecordSize);
  if(n!=int(RecordSize)||memcmp(record_,readback.bytes,RecordSize))return freeze(State::IoFrozen,n<0?n:-EIO);
  slot_=int8_t(next);wireVersion_=2;state_=State::Ready;return 0;
}
int PkiLedger::initialize(SettingsBackend &backend,const uint8_t localPublic[32],uint32_t local,bool quiescent){
  if(busy_)return -EBUSY;
  if(state_!=State::Empty)return -EACCES;
  if(!quiescent)return -EAGAIN;
  if(!node(local)||!publicKey(localPublic)||bench::MtSignature::nodeId(localPublic)!=local)return -EINVAL;
  Busy guard(busy_);memcpy(record_,"ZTPL",4);record_[4]=2;record_[5]=uint8_t(SendLimit);record_[6]=PeerLimit;record_[7]=ReceiveLimit;
  put32(record_+12,local);memcpy(record_+16,localPublic,32);return commit(backend);
}
int PkiLedger::peerIndex(uint32_t id)const{for(unsigned i=0;i<record_[49];++i)if(get32(peer(record_,i))==id)return int(i);return -1;}
bool PkiLedger::boundTo(const uint8_t localPublic[32],uint32_t local)const{
  return ready()&&localPublic&&get32(record_+12)==local&&!memcmp(record_+16,localPublic,32);
}
bool PkiLedger::lookup(uint32_t id,uint8_t outPublic[32])const{
  if(!ready()||!outPublic)return false;
  const int index=peerIndex(id);if(index<0)return false;
  const uint8_t *p=peer(record_,unsigned(index));if(p[37])return false;
  memcpy(outPublic,p+4,32);return true;
}
bool PkiLedger::revoked(uint32_t id)const {
  if(!ready())return false;
  const int index=peerIndex(id);return index>=0&&peer(record_,unsigned(index))[37]==1;
}
bool PkiLedger::singleActivePeer(uint32_t &out)const{
  if(!ready())return false;
  uint32_t chosen=0;
  for(unsigned i=0;i<record_[49];++i){
    const uint8_t *p=peer(record_,i);if(p[37])continue;
    if(chosen)return false;
    chosen=get32(p);
  }
  if(!chosen)return false;
  out=chosen;return true;
}
bool PkiLedger::singleStoredActivePeer(uint32_t &out)const{
  if(!ready()||record_[49]!=1)return false;
  const uint8_t *p=peer(record_,0);
  if(p[37])return false;
  out=get32(p);return out!=0;
}
bool PkiLedger::activePeers(uint32_t *out,size_t capacity,size_t &count)const{
  if(!ready()||!out)return false;
  uint32_t staged[PeerLimit]{};size_t n=0;
  for(unsigned i=0;i<record_[49];++i){const uint8_t *p=peer(record_,i);if(!p[37])staged[n++]=get32(p);}
  if(n>capacity)return false;
  memcpy(out,staged,n*sizeof(uint32_t));count=n;return true;
}
bool PkiLedger::status(Status &out)const{
  if(!ready()||(wireVersion_!=1&&wireVersion_!=2))return false;
  Status value;value.generation=get32(record_+8);value.sent=record_[48];
  value.format=wireVersion_;value.stored=record_[49];
  for(unsigned i=0;i<record_[49];++i){const uint8_t *p=peer(record_,i);value.received+=p[36];if(!p[37])++value.active;}
  out=value;return true;
}
bool PkiLedger::active(uint32_t id,const uint8_t pub[32])const{
  if(!ready()||!pub)return false;
  const int index=peerIndex(id);
  return index>=0&&!peer(record_,unsigned(index))[37]&&!memcmp(peer(record_,unsigned(index))+4,pub,32);
}
int PkiLedger::addTrustedPeer(SettingsBackend &backend,uint32_t id,const uint8_t pub[32],bool quiescent){
  if(busy_)return -EBUSY;
  if(!ready())return -EACCES;
  if(anchored_)return -EACCES;
  if(!quiescent)return -EAGAIN;
  if(!node(id)||id==get32(record_+12)||!publicKey(pub)||!memcmp(pub,record_+16,32))return -EINVAL;
  for(unsigned i=0;i<record_[49];++i){const uint8_t *p=peer(record_,i);
    if(get32(p)==id||!memcmp(p+4,pub,32))return -EEXIST;}
  if(record_[49]==PeerLimit)return -ENOSPC;
  Busy guard(busy_);uint8_t *p=peer(record_,record_[49]++);put32(p,id);memcpy(p+4,pub,32);return commit(backend);
}
int PkiLedger::revokeTrustedPeer(SettingsBackend &backend,uint32_t id,const uint8_t pub[32],bool quiescent){
  if(busy_)return -EBUSY;
  if(!ready()||!pub)return -EACCES;
  if(anchored_)return -EACCES;
  const int index=peerIndex(id);if(index<0)return -ENOENT;
  uint8_t *p=peer(record_,unsigned(index));if(memcmp(p+4,pub,32))return -EACCES;
  if(p[37])return 0; // Existing verified tombstone: idempotent, no write.
  if(!quiescent)return -EAGAIN;
  Busy guard(busy_);p[37]=1;return commit(backend);
}
bool PkiLedger::sent(uint32_t id)const{return !ready()||!id||hasId(record_+HeaderSize,record_[48],id);}
int PkiLedger::reserve(SettingsBackend &backend,uint32_t id,bool quiescent){
  if(busy_)return -EBUSY;
  if(!ready())return -EACCES;
  if(anchored_)return -EACCES;
  if(!id)return -EINVAL;
  if(sent(id))return -EALREADY;
  if(record_[48]==SendLimit)return -ENOSPC;
  if(!quiescent)return -EAGAIN;
  Busy guard(busy_);put32(record_+HeaderSize+size_t(record_[48]++)*4,id);return commit(backend);
}
ReplayCheck PkiLedger::seen(uint32_t id,const uint8_t pub[32],uint32_t packetId)const{
  if(!ready())return ReplayCheck::NotReady;
  if(!packetId||!pub||!node(id))return ReplayCheck::Invalid;
  const int index=peerIndex(id);if(index<0)return ReplayCheck::UnknownPeer;
  const uint8_t *p=peer(record_,unsigned(index));if(p[37])return ReplayCheck::UnknownPeer;
  if(memcmp(pub,p+4,32))return ReplayCheck::WrongKey;
  if(hasId(p+40,p[36],packetId))return ReplayCheck::Seen;
  return p[36]==ReceiveLimit?ReplayCheck::Full:ReplayCheck::Fresh;
}
int PkiLedger::commitAuthenticated(SettingsBackend &backend,uint32_t id,const uint8_t pub[32],uint32_t packetId,bool quiescent){
  if(busy_)return -EBUSY;
  if(anchored_)return -EACCES;
  const auto admission=seen(id,pub,packetId);
  if(admission==ReplayCheck::Seen)return -EALREADY;
  if(admission==ReplayCheck::Full)return -ENOSPC;
  if(admission!=ReplayCheck::Fresh)return -EACCES;
  if(!quiescent)return -EAGAIN;
  Busy guard(busy_);uint8_t *p=peer(record_,unsigned(peerIndex(id)));put32(p+40+size_t(p[36]++)*4,packetId);return commit(backend);
}
bool PkiLedger::close(){if(busy_)return false;freeze(State::Closed,0);return true;}

bool LedgerNonceAuthority::reserve(KeyRef ref,const uint8_t pub[32],uint32_t local,uint32_t id,uint32_t &extra){
  if(busy_||!identityMatches(identity_,ledger_,ref,pub,local))return false;
  Busy guard(busy_);
  if(ledger_.reserve(backend_,id,gate_.quiescent()))return false;
  Scratch<4> random;
  if(!entropy_.fill(random.bytes,4)||!identityMatches(identity_,ledger_,ref,pub,local))return false;
  extra=get32(random.bytes);return true;
}
bool LedgerReplayAuthority::identityBound()const{
  IdentityBinding b;KeyRef ref;
  return !busy_&&identity_.binding(b,ref)&&ledger_.boundTo(b.publicKey,b.node);
}
bool LedgerReplayAuthority::boundTo(const uint8_t pub[32],uint32_t local)const{return identityBound()&&ledger_.boundTo(pub,local);}
bool LedgerReplayAuthority::lookup(uint32_t id,uint8_t out[32])const{return identityBound()&&ledger_.lookup(id,out);}
bool LedgerReplayAuthority::revoked(uint32_t id)const{return identityBound()&&ledger_.revoked(id);}
bool LedgerReplayAuthority::active(uint32_t id,const uint8_t pub[32])const{return identityBound()&&ledger_.active(id,pub);}
ReplayCheck LedgerReplayAuthority::seen(uint32_t id,const uint8_t pub[32],uint32_t packetId)const{
  return identityBound()?ledger_.seen(id,pub,packetId):ReplayCheck::NotReady;
}
bool LedgerReplayAuthority::commitAuthenticated(uint32_t id,const uint8_t pub[32],uint32_t packetId){
  if(!identityBound())return false;
  IdentityBinding b;KeyRef ref;if(!identity_.binding(b,ref))return false;
  Busy guard(busy_);return ledger_.commitAuthenticated(backend_,id,pub,packetId,gate_.quiescent())==0&&
    identityMatches(identity_,ledger_,ref,b.publicKey,b.node);
}
bool LedgerReplayAuthority::revokeTrustedPeer(uint32_t id,const uint8_t pub[32]){
  if(!identityBound())return false;
  IdentityBinding b;KeyRef ref;if(!identity_.binding(b,ref))return false;
  Busy guard(busy_);return ledger_.revokeTrustedPeer(backend_,id,pub,gate_.quiescent())==0&&
    identityMatches(identity_,ledger_,ref,b.publicKey,b.node);
}
}
