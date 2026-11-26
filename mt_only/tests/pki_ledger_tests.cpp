// SPDX-License-Identifier: GPL-3.0-only
// Synthetic credentials, peers and RAM storage only. No devices or target RNG.
#include "pki/PkiLedger.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
static unsigned checks=0,cuts=0,bits=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();}}while(0)
static const char *keys[]={"mtpki/ledger0","mtpki/ledger1"};
static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^((0u-(c&1))&0xedb88320u);}return ~c;}
static void seal(Bytes &raw){put32(raw.data()+raw.size()-4,crc(raw.data(),raw.size()-4));}
struct Fixture {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Fixture(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+i*17);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  IdentityCandidate candidate()const{return IdentityCandidate(binding,scalar);}
};
struct Memory:mt::SettingsBackend {
  std::map<std::string,Bytes> values;unsigned reads=0,writes=0;int readError=0,writeError=0,cut=-1;
  bool writeBytes=true,alterReadback=false;int forcedLength=-999;std::function<void()> writing;
  int read(const char *key,uint8_t *out,size_t cap)override{
    ++reads;if(readError)return readError;
    if(forcedLength!=-999)return forcedLength;
    auto it=values.find(key);if(it==values.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    if(it->second.size())memcpy(out,it->second.data(),it->second.size());
    if(alterReadback&&it->second.size())out[0]^=1;
    return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t n)override{
    ++writes;if(writing)writing();
    CHECK(n==PkiLedger::RecordSize||n==IdentityJournal::RecordSize);
    if(writeBytes){size_t count=cut<0?n:size_t(cut);CHECK(count<=n);values[key]=Bytes(data,data+count);}
    return writeError;
  }
};
struct Gate:LedgerStorageGate {bool allowed=true;bool quiescent()const override{return allowed;}};
struct Random:EntropySource {
  unsigned calls=0;bool success=true;std::function<void()> entering;
  bool fill(uint8_t *out,size_t n)override{++calls;CHECK(n==4);if(entering)entering();memset(out,0x37,n);return success;}
};
static void bindIdentity(IdentityJournal &i,Memory &m,Fixture &f){MonocypherIdentityDeriver d;CHECK(!i.load(m,d));CHECK(!i.store(m,d,f.candidate(),true));}
static KeyRef refOf(IdentityJournal &i){IdentityBinding b;KeyRef r;CHECK(i.binding(b,r));return r;}
static Memory initialized(const Fixture &local){Memory m;PkiLedger l;CHECK(!l.load(m));CHECK(!l.initialize(m,local.binding.publicKey,local.binding.node,true));return m;}
static void disabled(PkiLedger &l,const Fixture &local,const Fixture &remote){
  CHECK(!l.ready()&&!l.boundTo(local.binding.publicKey,local.binding.node));uint8_t out[32];memset(out,0x55,32);
  CHECK(!l.lookup(remote.binding.node,out)&&out[0]==0x55);CHECK(!l.active(remote.binding.node,remote.binding.publicKey));
  CHECK(l.sent(42));CHECK(l.seen(remote.binding.node,remote.binding.publicKey,42)==ReplayCheck::NotReady);
}
static void frozen(PkiLedger &l,Memory &m,const Fixture &local,const Fixture &remote){
  disabled(l,local,remote);CHECK(l.reserve(m,123,true)==-EACCES);CHECK(l.load(m)==-EACCES);
  CHECK(l.initialize(m,local.binding.publicKey,local.binding.node,true)==-EACCES);
}
int main(){
  Fixture local(11),remote(59),another(107),fourth(149),fifth(193),sixth(227);
  const uint32_t peer=remote.binding.node;const uint8_t *pub=remote.binding.publicKey;
  {
    Memory m;PkiLedger l;disabled(l,local,remote);CHECK(l.reserve(m,1,true)==-EACCES);CHECK(!m.reads&&!m.writes);
    CHECK(!l.load(m));CHECK(m.reads==2&&!m.writes);CHECK(l.state()==PkiLedger::State::Empty);disabled(l,local,remote);
    CHECK(l.initialize(m,local.binding.publicKey,local.binding.node,false)==-EAGAIN);CHECK(!m.writes);
    CHECK(l.initialize(m,nullptr,local.binding.node,true)==-EINVAL);CHECK(l.initialize(m,pub,local.binding.node,true)==-EINVAL);
    CHECK(!l.initialize(m,local.binding.publicKey,local.binding.node,true));CHECK(m.writes==1);
    CHECK(l.boundTo(local.binding.publicKey,local.binding.node));CHECK(!l.boundTo(pub,peer));CHECK(!l.boundTo(nullptr,peer));
    CHECK(l.initialize(m,pub,peer,true)==-EACCES);CHECK(l.load(m)==-EACCES);
    CHECK(l.addTrustedPeer(m,peer,pub,false)==-EAGAIN);CHECK(l.addTrustedPeer(m,local.binding.node,pub,true)==-EINVAL);
    CHECK(l.addTrustedPeer(m,peer,local.binding.publicKey,true)==-EINVAL);CHECK(!l.addTrustedPeer(m,peer,pub,true));
    CHECK(l.active(peer,pub));uint8_t copy[32]{};CHECK(l.lookup(peer,copy)&&!memcmp(copy,pub,32));
    CHECK(l.addTrustedPeer(m,peer,pub,true)==-EEXIST);CHECK(l.addTrustedPeer(m,peer,another.binding.publicKey,true)==-EEXIST);
    CHECK(l.addTrustedPeer(m,another.binding.node,pub,true)==-EEXIST);CHECK(m.writes==2);
    CHECK(l.seen(another.binding.node,another.binding.publicKey,7)==ReplayCheck::UnknownPeer);
    CHECK(l.seen(peer,another.binding.publicKey,7)==ReplayCheck::WrongKey);CHECK(l.seen(peer,pub,0)==ReplayCheck::Invalid);
    CHECK(l.commitAuthenticated(m,peer,another.binding.publicKey,7,true)==-EACCES);CHECK(m.writes==2);
    CHECK(l.commitAuthenticated(m,peer,pub,7,false)==-EAGAIN);CHECK(l.seen(peer,pub,7)==ReplayCheck::Fresh);
    CHECK(!l.commitAuthenticated(m,peer,pub,7,true));CHECK(l.seen(peer,pub,7)==ReplayCheck::Seen);
    CHECK(l.commitAuthenticated(m,peer,pub,7,true)==-EALREADY);CHECK(m.writes==3);
    CHECK(l.reserve(m,0,true)==-EINVAL);CHECK(l.reserve(m,123,false)==-EAGAIN);CHECK(!l.sent(123));
    CHECK(!l.reserve(m,123,true));CHECK(l.sent(123));CHECK(l.reserve(m,123,true)==-EALREADY);CHECK(m.writes==4);
    PkiLedger restarted;CHECK(!restarted.load(m));CHECK(restarted.sent(123));CHECK(restarted.seen(peer,pub,7)==ReplayCheck::Seen);
    CHECK(restarted.active(peer,pub));CHECK(restarted.close());disabled(restarted,local,remote);CHECK(restarted.load(m)==-EACCES);
  }
  // Every byte interruption on initialize, trust pin, outbound ID, inbound ID,
  // inactive-slot overwrite, and permanent revocation. Previous slot preserved.
  for(unsigned path=0;path<6;++path){
    Memory base;if(path)base=initialized(local);
    if(path>=2){PkiLedger l;CHECK(!l.load(base));CHECK(!l.addTrustedPeer(base,peer,pub,true));}
    if(path==4){PkiLedger l;CHECK(!l.load(base));CHECK(!l.reserve(base,111,true));}
    if(path==5){PkiLedger l;CHECK(!l.load(base));CHECK(!l.commitAuthenticated(base,peer,pub,444,true));}
    for(unsigned cut=0;cut<=PkiLedger::RecordSize;++cut){
      ++cuts;Memory m=base;PkiLedger l;CHECK(!l.load(m));
      std::string previous;uint32_t generation=0;Bytes old;
      for(auto &entry:base.values){const uint32_t g=get32(entry.second.data()+8);if(g>generation){generation=g;previous=entry.first;old=entry.second;}}
      m.cut=int(cut);m.writeError=-EIO;int rc=0;
      if(path==0)rc=l.initialize(m,local.binding.publicKey,local.binding.node,true);
      if(path==1)rc=l.addTrustedPeer(m,peer,pub,true);
      if(path==2||path==4)rc=l.reserve(m,222,true);
      if(path==3)rc=l.commitAuthenticated(m,peer,pub,333,true);
      if(path==5)rc=l.revokeTrustedPeer(m,peer,pub,true);
      CHECK(rc==-EIO);CHECK(l.state()==PkiLedger::State::IoFrozen);frozen(l,m,local,remote);
      if(path)CHECK(m.values[previous]==old);
      PkiLedger restart;CHECK(restart.load(m)==(cut==PkiLedger::RecordSize?0:-EILSEQ));
      if(cut==PkiLedger::RecordSize){CHECK(restart.boundTo(local.binding.publicKey,local.binding.node));
        if(path>=1&&path!=5)CHECK(restart.active(peer,pub));
        if(path==2||path==4)CHECK(restart.sent(222));
        if(path==3)CHECK(restart.seen(peer,pub,333)==ReplayCheck::Seen);
        if(path==5){CHECK(!restart.active(peer,pub));CHECK(restart.seen(peer,pub,444)==ReplayCheck::UnknownPeer);}
      }else CHECK(restart.state()==PkiLedger::State::CorruptFrozen);
    }
  }
  Memory pinned=initialized(local);{PkiLedger l;CHECK(!l.load(pinned));CHECK(!l.addTrustedPeer(pinned,peer,pub,true));}
  for(unsigned slot=0;slot<2;++slot)for(unsigned bit=0;bit<PkiLedger::RecordSize*8;++bit){
    ++bits;Memory m=pinned;m.values[keys[slot]][bit/8]^=uint8_t(1u<<(bit%8));PkiLedger l;
    CHECK(l.load(m)==-EILSEQ);CHECK(l.state()==PkiLedger::State::CorruptFrozen);frozen(l,m,local,remote);
  }
  for(int error:{-EIO,-ENOMEM,-EAGAIN}){Memory m=pinned;m.readError=error;PkiLedger l;CHECK(l.load(m)==error);frozen(l,m,local,remote);}
  for(int size:{-EIO,-ENOENT,-E2BIG,0,1751,1753}){
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));m.forcedLength=size;CHECK(l.reserve(m,88,true)<0);frozen(l,m,local,remote);
    m.forcedLength=-999;PkiLedger fresh;CHECK(!fresh.load(m));CHECK(fresh.sent(88));
  }
  for(unsigned fault=0;fault<4;++fault){
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));const auto old=m.values;
    if(fault==0){m.writeBytes=false;m.writeError=-EIO;}
    if(fault==1)m.writeBytes=false; // stale previous slot readback, not just absence
    if(fault==2)m.alterReadback=true;
    if(fault==3)m.writeError=1;
    CHECK(l.reserve(m,89,true)<0);frozen(l,m,local,remote);
    if(fault<2)CHECK(m.values==old);
  }
  // CRC-valid malformed records and history rewrites remain disabled.
  for(unsigned mode=0;mode<13;++mode){
    Memory m=pinned;Bytes &r=m.values[keys[1]];uint8_t *p=r.data()+PkiLedger::PeerOffset;
    switch(mode){
      case 0:r[4]=3;break;case 1:r[5]=127;break;case 2:r[6]=3;break;case 3:r[7]=63;break;
      case 4:r[50]=1;break;case 5:r[48]=129;break;case 6:r[49]=5;break;case 7:put32(r.data()+8,UINT32_MAX);break;
      case 8:put32(r.data()+12,local.binding.node^8);break;case 9:p[37]=1;break;
      case 10:p[36]=uint8_t(PkiLedger::ReceiveLimit+1);break;case 11:put32(p+40,9);break;case 12:put32(p,0xffffffffu);break;
    }seal(r);PkiLedger l;CHECK(l.load(m)==-EILSEQ);
  }
  {
    Memory m=pinned;m.values.erase(keys[0]);PkiLedger l;CHECK(l.load(m)==-EILSEQ);
    m=pinned;m.values[keys[0]]=m.values[keys[1]];PkiLedger same;CHECK(!same.load(m));CHECK(same.active(peer,pub));
    m=pinned;std::swap(m.values[keys[0]],m.values[keys[1]]);PkiLedger reversed;CHECK(!reversed.load(m));CHECK(reversed.active(peer,pub));
  }
  {
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));CHECK(!l.reserve(m,91,true));CHECK(!l.reserve(m,92,true));
    // Each record is individually valid, but successor silently rewrites old ID.
    Bytes &newest=m.values[keys[1]];put32(newest.data()+PkiLedger::HeaderSize,99);seal(newest);
    PkiLedger bad;CHECK(bad.load(m)==-EILSEQ);
  }
  {
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));CHECK(!l.reserve(m,91,true));
    Bytes &newest=m.values[keys[0]];memcpy(newest.data()+PkiLedger::PeerOffset+4,another.binding.publicKey,32);
    put32(newest.data()+PkiLedger::PeerOffset,another.binding.node);seal(newest);PkiLedger bad;CHECK(bad.load(m)==-EILSEQ);
  }
  {
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));CHECK(!l.reserve(m,91,true));
    Bytes &newest=m.values[keys[0]];memcpy(newest.data()+16,another.binding.publicKey,32);put32(newest.data()+12,another.binding.node);
    seal(newest);PkiLedger bad;CHECK(bad.load(m)==-EILSEQ);
  }
  for(unsigned mutation=0;mutation<2;++mutation){
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));CHECK(!l.revokeTrustedPeer(m,peer,pub,true));CHECK(!l.reserve(m,91,true));
    Bytes &newest=m.values[keys[1]];uint8_t *entry=newest.data()+PkiLedger::PeerOffset;
    if(!mutation){entry[37]=0;newest[48]=2;put32(newest.data()+PkiLedger::HeaderSize+4,92);}
    else {newest[48]=0;put32(newest.data()+PkiLedger::HeaderSize,0);entry[36]=1;put32(entry+40,92);}
    seal(newest);PkiLedger bad;CHECK(bad.load(m)==-EILSEQ); // un-revoke or grow RX after revocation
  }
  {
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));CHECK(!l.commitAuthenticated(m,peer,pub,901,true));
    CHECK(l.revokeTrustedPeer(m,peer,pub,false)==-EAGAIN);CHECK(l.active(peer,pub));
    CHECK(l.revokeTrustedPeer(m,peer,another.binding.publicKey,true)==-EACCES);CHECK(l.active(peer,pub));
    CHECK(!l.revokeTrustedPeer(m,peer,pub,true));const auto writes=m.writes;uint8_t out[32];memset(out,0x44,32);
    CHECK(!l.lookup(peer,out)&&out[0]==0x44);CHECK(!l.active(peer,pub));CHECK(l.seen(peer,pub,901)==ReplayCheck::UnknownPeer);
    CHECK(l.commitAuthenticated(m,peer,pub,902,true)==-EACCES);CHECK(!l.revokeTrustedPeer(m,peer,pub,false));CHECK(m.writes==writes);
    CHECK(l.addTrustedPeer(m,peer,pub,true)==-EEXIST);CHECK(l.addTrustedPeer(m,another.binding.node,pub,true)==-EEXIST);
    PkiLedger reboot;CHECK(!reboot.load(m));CHECK(!reboot.active(peer,pub));CHECK(reboot.addTrustedPeer(m,peer,pub,true)==-EEXIST);
    // Tombstone retains history physically; no new export API is needed.
    const Bytes *last=nullptr;for(auto &entry:m.values)if(!last||get32(entry.second.data()+8)>get32(last->data()+8))last=&entry.second;
    CHECK(last&&(*last)[PkiLedger::PeerOffset+36]==1&&get32(last->data()+PkiLedger::PeerOffset+40)==901);
  }
  {
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));
    for(unsigned i=1;i<=PkiLedger::SendLimit;++i)CHECK(!l.reserve(m,i,true));
    const auto writes=m.writes;CHECK(l.reserve(m,PkiLedger::SendLimit+1,true)==-ENOSPC);CHECK(l.reserve(m,1,true)==-EALREADY);CHECK(m.writes==writes);
    PkiLedger reloaded;CHECK(!reloaded.load(m));for(unsigned i=1;i<=PkiLedger::SendLimit;++i)CHECK(reloaded.sent(i));
    CHECK(reloaded.reserve(m,PkiLedger::SendLimit+2,true)==-ENOSPC);
    for(auto *f:{&another,&fourth,&fifth})CHECK(!reloaded.addTrustedPeer(m,f->binding.node,f->binding.publicKey,true));
    CHECK(reloaded.addTrustedPeer(m,sixth.binding.node,sixth.binding.publicKey,true)==-ENOSPC);
    for(auto *f:{&remote,&another,&fourth,&fifth}){
      for(unsigned i=1;i<=PkiLedger::ReceiveLimit;++i)CHECK(!reloaded.commitAuthenticated(m,f->binding.node,f->binding.publicKey,i,true));
      CHECK(reloaded.seen(f->binding.node,f->binding.publicKey,PkiLedger::ReceiveLimit+1)==ReplayCheck::Full);
      CHECK(reloaded.seen(f->binding.node,f->binding.publicKey,1)==ReplayCheck::Seen);
      CHECK(reloaded.commitAuthenticated(m,f->binding.node,f->binding.publicKey,PkiLedger::ReceiveLimit+1,true)==-ENOSPC);
    }
    PkiLedger full;CHECK(!full.load(m));CHECK(full.sent(128));CHECK(full.seen(peer,pub,64)==ReplayCheck::Seen);
    CHECK(full.seen(peer,pub,PkiLedger::ReceiveLimit+1)==ReplayCheck::Full);CHECK(full.reserve(m,PkiLedger::SendLimit+3,true)==-ENOSPC);
    for(auto *f:{&remote,&another,&fourth,&fifth})CHECK(!full.revokeTrustedPeer(m,f->binding.node,f->binding.publicKey,true));
    CHECK(full.addTrustedPeer(m,sixth.binding.node,sixth.binding.publicKey,true)==-ENOSPC);
    PkiLedger revokedFull;CHECK(!revokedFull.load(m));CHECK(!revokedFull.active(peer,pub));CHECK(revokedFull.sent(128));
  }
  {
    Memory identityMemory,m=pinned;IdentityJournal identity;bindIdentity(identity,identityMemory,local);
    PkiLedger l;CHECK(!l.load(m));Gate gate;LedgerReplayAuthority replay(identity,l,m,gate);
    gate.allowed=false;CHECK(!replay.revokeTrustedPeer(peer,pub));CHECK(replay.active(peer,pub));gate.allowed=true;
    CHECK(replay.revokeTrustedPeer(peer,pub));CHECK(!replay.active(peer,pub));CHECK(replay.revokeTrustedPeer(peer,pub));
    PkiLedger restart;CHECK(!restart.load(m));LedgerReplayAuthority after(identity,restart,m,gate);CHECK(!after.active(peer,pub));
  }
  {
    Memory identityMemory,ledgerMemory=pinned;IdentityJournal identity;bindIdentity(identity,identityMemory,local);
    PkiLedger l;CHECK(!l.load(ledgerMemory));Gate gate;Random rng;LedgerNonceAuthority nonce(identity,l,ledgerMemory,rng,gate);
    LedgerReplayAuthority replay(identity,l,ledgerMemory,gate);auto key=refOf(identity);uint32_t extra=0xdeadbeef;
    CHECK(replay.boundTo(local.binding.publicKey,local.binding.node));CHECK(replay.active(peer,pub));
    CHECK(!nonce.reserve({key.slot,key.generation+1},local.binding.publicKey,local.binding.node,500,extra));CHECK(!rng.calls);
    CHECK(!nonce.reserve(key,pub,local.binding.node,500,extra));CHECK(!nonce.reserve(key,local.binding.publicKey,peer,500,extra));CHECK(!rng.calls);
    gate.allowed=false;CHECK(!nonce.reserve(key,local.binding.publicKey,local.binding.node,500,extra));CHECK(extra==0xdeadbeef&&!l.sent(500));
    CHECK(!replay.commitAuthenticated(peer,pub,600));CHECK(replay.seen(peer,pub,600)==ReplayCheck::Fresh);gate.allowed=true;
    rng.entering=[&]{CHECK(l.sent(500));CHECK(!nonce.reserve(key,local.binding.publicKey,local.binding.node,501,extra));};
    rng.success=false;CHECK(!nonce.reserve(key,local.binding.publicKey,local.binding.node,500,extra));CHECK(extra==0xdeadbeef&&l.sent(500));
    rng.entering={};rng.success=true;CHECK(!nonce.reserve(key,local.binding.publicKey,local.binding.node,500,extra));CHECK(rng.calls==1);
    CHECK(nonce.reserve(key,local.binding.publicKey,local.binding.node,501,extra));CHECK(extra==0x37373737&&rng.calls==2);
    // Same-scalar metadata generation does not create a fresh nonce domain.
    MonocypherIdentityDeriver d;Fixture rotated=local;rotated.binding.generation=2;CHECK(!identity.store(identityMemory,d,rotated.candidate(),true));
    const auto newKey=refOf(identity);CHECK(!nonce.reserve(key,local.binding.publicKey,local.binding.node,502,extra));
    CHECK(!nonce.reserve(newKey,local.binding.publicKey,local.binding.node,501,extra));CHECK(nonce.reserve(newKey,local.binding.publicKey,local.binding.node,502,extra));
    CHECK(replay.commitAuthenticated(peer,pub,600));CHECK(replay.seen(peer,pub,600)==ReplayCheck::Seen);
    CHECK(!replay.commitAuthenticated(peer,pub,600));CHECK(!replay.commitAuthenticated(peer,another.binding.publicKey,601));
    // A fresh explicitly imported same key/identity object still sees old history.
    Memory importedMemory;IdentityJournal imported;bindIdentity(imported,importedMemory,local);PkiLedger restart;CHECK(!restart.load(ledgerMemory));
    LedgerNonceAuthority importedNonce(imported,restart,ledgerMemory,rng,gate);LedgerReplayAuthority importedReplay(imported,restart,ledgerMemory,gate);
    CHECK(!importedNonce.reserve(refOf(imported),local.binding.publicKey,local.binding.node,500,extra));
    CHECK(importedReplay.seen(peer,pub,600)==ReplayCheck::Seen);
    Memory otherIdentityMemory;IdentityJournal other;bindIdentity(other,otherIdentityMemory,another);
    LedgerNonceAuthority wrong(other,restart,ledgerMemory,rng,gate);LedgerReplayAuthority wrongReplay(other,restart,ledgerMemory,gate);
    CHECK(!wrong.reserve(refOf(other),another.binding.publicKey,another.binding.node,900,extra));CHECK(!wrongReplay.active(peer,pub));
    CHECK(wrongReplay.seen(peer,pub,600)==ReplayCheck::NotReady);
    ledgerMemory.writeError=-EIO;CHECK(!importedNonce.reserve(refOf(imported),local.binding.publicKey,local.binding.node,700,extra));
    CHECK(!importedReplay.boundTo(local.binding.publicKey,local.binding.node));CHECK(!importedReplay.active(peer,pub));
    CHECK(importedReplay.seen(peer,pub,600)==ReplayCheck::NotReady);
  }
  // Reentrant storage callbacks cannot expose tentative reservations/trust.
  {
    Memory m=pinned;PkiLedger l;CHECK(!l.load(m));m.writing=[&]{CHECK(!l.ready());CHECK(!l.close());
      CHECK(l.reserve(m,1,true)==-EBUSY);CHECK(l.load(m)==-EBUSY);CHECK(!l.active(peer,pub));};
    CHECK(!l.reserve(m,777,true));CHECK(l.sent(777));
  }
  std::printf("{\"PASS\":true,\"assertions\":%u,\"interruption_cut_points\":%u,\"corrupt_single_bits\":%u,\"record_size\":%zu,\"ledger_object_size\":%zu}\n",checks,cuts,bits,PkiLedger::RecordSize,sizeof(PkiLedger));
  return 0;
}
