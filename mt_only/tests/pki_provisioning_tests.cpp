// SPDX-License-Identifier: GPL-3.0-only
// Host-only public deterministic fixtures; no device, live RNG or credentials.
#include "pki/PkiProvisioning.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <vector>
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
using Rows=std::map<std::string,Bytes>;
struct Memory:mt::SettingsBackend {
  Rows rows;unsigned writes=0;int fault=0;unsigned at=0;size_t cut=0;std::vector<std::string> order;
  std::function<void()> onWrite;
  int read(const char *key,uint8_t *out,size_t capacity)override{
    auto i=rows.find(key);if(i==rows.end())return -ENOENT;
    if(!i->second.empty())memcpy(out,i->second.data(),std::min(capacity,i->second.size()));
    return int(i->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t n)override{
    ++writes;order.emplace_back(key);if(onWrite)onWrite();
    if(writes==at&&fault==1)return -EIO;
    rows[key]={data,data+((writes==at&&fault==3)?std::min(cut,n):n)};
    return writes==at&&fault==2?-EIO:0;
  }
};
struct Boot:ProvisioningSettings {
  Memory &memory;Rows snapshot;bool held=false;unsigned reads=0,begins=0;int error=0;
  std::function<void()> onBegin,onRead;
  explicit Boot(Memory &m):memory(m){}
  int begin()override{assert(!held);++begins;if(onBegin)onBegin();if(error)return error;held=true;snapshot=memory.rows;return 0;}
  void end()override{assert(held);held=false;snapshot.clear();}
  int read(const char *key,uint8_t *out,size_t capacity)override{
    assert(held);++reads;if(onRead)onRead();
    if(memory.at&&memory.at==memory.writes&&memory.fault==4)return -EIO;
    auto i=snapshot.find(key);if(i==snapshot.end())return -ENOENT;
    if(!i->second.empty())memcpy(out,i->second.data(),std::min(capacity,i->second.size()));
    if(memory.at&&memory.at==memory.writes&&memory.fault==5&&capacity)out[0]^=1;
    return int(i->second.size());
  }
  int persist(const char *key,const uint8_t *data,size_t size)override{return memory.write(key,data,size);}
};
struct Context:ProvisioningContext,PkiStorageContext {
  bool permit=true,lease=true,candidate=true,peer=true;uint64_t now=100;unsigned depth=0;
  std::function<void()> onCandidate,onPeer;
  bool tryEnter()override{if(!lease)return false;++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool quiescent()const override{return depth&&permit;}
  bool bootQuiescent()const override{return quiescent();}
  uint64_t millis()const override{return now;}
  bool candidateAllowed(const IdentityBinding &)const override{if(onCandidate)onCandidate();return candidate;}
  bool peerAllowed(const IdentityBinding &,const ProvisionPeer &)const override{if(onPeer)onPeer();return peer;}
};
struct Entropy:EntropySource {
  uint8_t raw[32]{};unsigned calls=0;bool fail=false;std::function<void()> hook;
  explicit Entropy(unsigned seed=19){for(unsigned i=0;i<32;++i)raw[i]=uint8_t(seed+23*i);}
  bool fill(uint8_t *out,size_t n)override{assert(n==32);++calls;memcpy(out,raw,n);if(hook)hook();return !fail;}
};
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+23*i);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  ~Key(){eraseIdentityBytes(scalar,32);}
  ProvisionPeer pin()const{ProvisionPeer p;p.node=binding.node;memcpy(p.publicKey,binding.publicKey,32);return p;}
  IdentityCandidate candidate()const{return IdentityCandidate(binding,scalar);}
};
struct Fixture {
  Memory memory;Boot boot{memory};Context context;Entropy entropy;Key local{19},peer{71};
  explicit Fixture(unsigned seed=19):entropy(seed){memory.rows["mtbase/state0"]=Bytes(112,0xab);memory.rows["mtbase/nodes0"]=Bytes(1492,0x56);memory.rows["mtbase/config"]=Bytes(80,0x7f);}
  Rows publicRows()const{Rows out;for(const auto &row:memory.rows)if(row.first.rfind("mtbase/",0)==0)out.insert(row);return out;}
};
bool containsBytes(const PkiProvisioning &p,const uint8_t bytesToFind[32]){
  const auto *bytes=reinterpret_cast<const uint8_t *>(&p);
  return std::search(bytes,bytes+sizeof(p),bytesToFind,bytesToFind+32)!=bytes+sizeof(p);
}
bool containsScalar(const PkiProvisioning &p,const Key &key){return containsBytes(p,key.scalar);}
void pass(const char *s){std::cout<<"PASS "<<s<<'\n';}
void complete(Fixture &f){PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;assert(!p.stage(out));assert(!p.commit(out,f.peer.pin()));}
void restart(Fixture &f,bool mustComplete=false){
  f.memory.fault=0;f.context.permit=true;f.context.now=100;
  PkiProvisioning fresh(f.boot,f.context,f.entropy);ProvisionStatus status;
  const int rc=fresh.inspect(status);
  if(mustComplete)assert(!rc&&status.stored==ProvisionStored::Complete&&status.hasBinding&&status.hasPeer);
  if(status.stored!=ProvisionStored::Empty){unsigned draws=f.entropy.calls,writes=f.memory.writes;
    IdentityBinding out;out.node=123;assert(fresh.stage(out)<0&&out.node==123);
    assert(f.entropy.calls==draws&&f.memory.writes==writes);}
}
int main(){
  static_assert(std::is_constructible_v<PkiProvisioning,ProvisioningSettings &,ProvisioningContext &,EntropySource &>);
  static_assert(!std::is_constructible_v<PkiProvisioning,BootSettings &,mt::SettingsBackend &,ProvisioningContext &,EntropySource &>);
  {Fixture f;const auto rows=f.memory.rows;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;
   assert(f.boot.reads==0&&f.entropy.calls==0&&f.memory.writes==0);ProvisionStatus empty;
   assert(!p.inspect(empty)&&empty.stored==ProvisionStored::Empty&&!empty.hasBinding&&!empty.hasPeer);
   assert(!p.stage(out)&&f.entropy.calls==1&&out.generation==1&&out.node==f.local.binding.node&&containsScalar(p,f.local));
   IdentityBinding same;assert(!p.stage(same)&&!memcmp(out.publicKey,same.publicKey,32)&&f.entropy.calls==1);
   assert(f.memory.rows==rows);assert(!p.commit(out,f.peer.pin())&&p.state()==ProvisionState::Committed&&!containsScalar(p,f.local));
   assert((f.memory.order==std::vector<std::string>{"mtpki/ledger0","mtpki/ledger1","mtidentity/state0"}));
   assert(f.memory.rows.count("mtidentity/state1")==0&&f.memory.writes==3);ProvisionStatus status;
   assert(!p.inspect(status)&&status.attempt==ProvisionState::Committed&&status.stored==ProvisionStored::Complete);
   assert(status.local.node==out.node&&status.peer.node==f.peer.binding.node&&!memcmp(status.peer.publicKey,f.peer.binding.publicKey,32));
   assert(p.commit(out,f.peer.pin())<0&&p.stage(out)<0&&f.entropy.calls==1&&f.memory.writes==3);restart(f,true);}
  pass("passive-two-stage-public-only-exact-order-readbacks-and-complete-restart");
  // Every preexisting slot, including malformed/oversized data, denies generation.
  for(const char *key:{"mtidentity/state0","mtidentity/state1","mtpki/ledger0","mtpki/ledger1"})for(size_t n:{size_t(0),size_t(1),size_t(84),size_t(1753)}){
    Fixture f;f.memory.rows[key]=Bytes(n,0x5a);const auto rows=f.memory.rows;
    PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;out.node=123;
    assert(p.stage(out)<0&&out.node==123&&!f.entropy.calls&&!f.memory.writes&&f.memory.rows==rows&&!containsScalar(p,f.local));
  }
  pass("all-four-preexisting-corrupt-oversized-slots-refuse-entropy-or-overwrite");
  {Fixture f;Entropy secondEntropy(43);PkiProvisioning first(f.boot,f.context,f.entropy),second(f.boot,f.context,secondEntropy);
   IdentityBinding a,b;assert(!first.stage(a)&&!second.stage(b));assert(!first.commit(a,f.peer.pin()));
   const auto committed=f.memory.rows;const auto writes=f.memory.writes;IdentityBinding retry;retry.node=123;
   assert(second.stage(retry)==-EEXIST&&retry.node==123&&second.state()==ProvisionState::Rejected);
   assert(f.memory.rows==committed&&f.memory.writes==writes&&secondEntropy.calls==1&&!containsBytes(second,secondEntropy.raw));}
  pass("staged-candidate-rechecks-empty-store-and-cannot-revive-after-peer-commit");
  for(unsigned variant=0;variant<9;++variant){Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;
    assert(!p.stage(out));const auto rows=f.memory.rows;ProvisionPeer peer=f.peer.pin();
    if(variant==0)++out.version;
    if(variant==1)++out.algorithm;
    if(variant==2)++out.generation;
    if(variant==3)++out.node;
    if(variant==4)out.publicKey[4]^=1;
    if(variant==5)peer=f.local.pin();
    if(variant==6)peer.node=out.node;
    if(variant==7)memset(peer.publicKey,0,32);
    if(variant==8){peer.publicKey[31]|=128;peer.node=bench::MtSignature::nodeId(peer.publicKey);}
    assert(p.commit(out,peer)<0&&p.state()==ProvisionState::Rejected&&!containsScalar(p,f.local));
    assert(f.memory.rows==rows&&f.memory.writes==0&&f.entropy.calls==1);assert(p.stage(out)<0&&p.commit(out,peer)<0);
  }
  pass("full-confirmation-fields-canonical-peer-crc-self-and-key-collision-terminal");
  for(unsigned variant=0;variant<4;++variant){Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;out.node=123;
    if(variant==0)f.context.candidate=false;
    int staged=p.stage(out);if(variant==0){assert(staged<0&&out.node==123);}else{
      assert(!staged);if(variant==1)f.context.candidate=false;if(variant==2)f.context.peer=false;
      if(variant==3)f.boot.onRead=[&]{f.context.peer=false;};
      assert(p.commit(out,f.peer.pin())==-EEXIST);
    }
    assert(f.memory.writes==0&&!containsScalar(p,f.local));
  }
  pass("nodecache-hardware-reserved-policy-rechecked-after-fresh-read-before-writes");
  {Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;out.node=123;f.context.lease=false;
   assert(p.stage(out)==-EBUSY&&out.node==123);f.context.lease=true;assert(p.state()==ProvisionState::New);
   f.entropy.hook=[&]{IdentityBinding other;assert(p.stage(other)==-EBUSY&&!p.cancel()&&p.tick()==-EBUSY);};
   assert(!p.stage(out));f.memory.onWrite=[&]{assert(p.commit(out,f.peer.pin())==-EBUSY&&!p.cancel());};
   assert(!p.commit(out,f.peer.pin())&&f.context.depth==0);}
  pass("lease-contention-and-callback-reentry-never-start-another-attempt");
  for(unsigned variant=0;variant<8;++variant){Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;out.node=123;
    if(variant==0){f.entropy.fail=true;assert(p.stage(out)<0&&out.node==123);}
    if(variant==1){f.entropy.hook=[&]{f.context.now+=PkiProvisioning::StageLifetimeMs;};assert(p.stage(out)==-ETIMEDOUT&&out.node==123);}
    if(variant==2){f.entropy.hook=[&]{f.context.permit=false;};assert(p.stage(out)<0&&out.node==123);}
    if(variant>=3){assert(!p.stage(out));
      if(variant==3){f.context.now+=PkiProvisioning::StageLifetimeMs;assert(p.tick()==-ETIMEDOUT);}
      if(variant==4){assert(p.cancel()&&p.state()==ProvisionState::Cancelled);}
      if(variant==5){f.context.permit=false;assert(p.tick()<0);}
      if(variant==6){f.boot.onRead=[&]{f.context.now+=PkiProvisioning::StageLifetimeMs;};assert(p.commit(out,f.peer.pin())<0);}
      if(variant==7){f.boot.onRead=[&]{f.context.now+=PkiProvisioning::StageLifetimeMs;};ProvisionStatus status;assert(p.inspect(status)<0);}
    }
    assert(!containsScalar(p,f.local)&&!containsBytes(p,f.entropy.raw)&&!f.memory.writes&&f.entropy.calls==1);assert(p.stage(out)<0);
  }
  {Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;
   assert(!p.stage(out));f.context.now+=(UINT64_C(1)<<32)+1;assert(p.tick()==-ETIMEDOUT&&!containsScalar(p,f.local));}
  {Fixture f;f.context.now=UINT64_MAX-20;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;
   assert(!p.stage(out));f.context.now+=119999;assert(!p.tick());++f.context.now;assert(p.tick()==-ETIMEDOUT&&!containsScalar(p,f.local));}
  pass("entropy-failure-gate-loss-cancel-expiry-64-bit-full-cycle-and-boundary-wrap-wipe");
  // Every exposed write/readback failure at each of the three durable steps.
  for(unsigned at=1;at<=3;++at)for(int fault:{1,2,4,5}){
    Fixture f;auto pub=f.publicRows();PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;assert(!p.stage(out));
    f.memory.at=at;f.memory.fault=fault;assert(p.commit(out,f.peer.pin())<0);
    assert(p.state()==ProvisionState::Frozen&&!containsScalar(p,f.local)&&f.memory.writes==at&&f.publicRows()==pub);
    unsigned writes=f.memory.writes;assert(p.stage(out)<0&&p.commit(out,f.peer.pin())<0&&f.memory.writes==writes);
    if(at<3)assert(!f.memory.rows.count("mtidentity/state0"));
    restart(f,at==3&&fault!=1);
  }
  pass("three-step-before-after-write-readback-error-mismatch-terminal-and-restart");
  unsigned cuts=0;
  for(unsigned at=1;at<=3;++at){const size_t length=at==3?IdentityJournal::RecordSize:PkiLedger::RecordSize;
    for(size_t cut=0;cut<length;++cut){Fixture f;const auto pub=f.publicRows();PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;
      assert(!p.stage(out));f.memory.at=at;f.memory.fault=3;f.memory.cut=cut;
      assert(p.commit(out,f.peer.pin())<0&&p.state()==ProvisionState::Frozen&&!containsScalar(p,f.local));
      assert(f.publicRows()==pub);if(at<3)assert(!f.memory.rows.count("mtidentity/state0"));restart(f);++cuts;
    }
  }
  std::cout<<"PASS every truncated persistent prefix: "<<cuts<<" cut points, no restart overwrite\n";
  for(unsigned at=1;at<=3;++at)for(unsigned where=0;where<3;++where){Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;assert(!p.stage(out));
    auto lose=[&]{if(f.memory.writes==at){if(where==2)f.context.now+=PkiProvisioning::StageLifetimeMs;else f.context.permit=false;}};
    if(where==0)f.memory.onWrite=lose;else f.boot.onRead=lose;
    assert(p.commit(out,f.peer.pin())<0&&p.state()==ProvisionState::Frozen&&!containsScalar(p,f.local));
    assert(f.memory.writes==at);}
  pass("gate-or-expiry-loss-after-each-write-or-readback-never-reports-success");
  {Fixture f;PkiProvisioning p(f.boot,f.context,f.entropy);IdentityBinding out;assert(!p.stage(out));
   f.memory.rows["mtidentity/state1"]=Bytes(84,0x22);const auto changed=f.memory.rows;
   assert(p.commit(out,f.peer.pin())<0&&f.memory.rows==changed&&!f.memory.writes&&!containsScalar(p,f.local));}
  pass("store-changing-between-stage-and-commit-preserved-and-rejected");
  {
    Fixture f;PkiProvisioning observer(f.boot,f.context,f.entropy);ProvisionStatus denied;
    denied.attempt=ProvisionState::Committed;denied.stored=ProvisionStored::Complete;denied.error=77;denied.hasBinding=true;denied.local.node=123;
    f.context.permit=false;assert(observer.inspect(denied)==-EACCES&&denied.attempt==ProvisionState::Committed&&
      denied.stored==ProvisionStored::Complete&&denied.error==77&&denied.hasBinding&&denied.local.node==123);
  }
  {
    Fixture f;f.memory.rows["mtpki/ledger0"]=Bytes(1,0x5a);PkiProvisioning observer(f.boot,f.context,f.entropy);ProvisionStatus status;
    assert(observer.inspect(status)==-EILSEQ&&status.stored==ProvisionStored::Corrupt&&status.error==-EILSEQ&&!status.hasBinding&&!status.hasPeer);
  }
  {
    Fixture f;complete(f);f.memory.rows.erase("mtidentity/state0");f.memory.rows.erase("mtidentity/state1");
    PkiProvisioning observer(f.boot,f.context,f.entropy);ProvisionStatus status;
    assert(!observer.inspect(status)&&status.stored==ProvisionStored::Partial&&!status.hasBinding&&!status.hasPeer);
  }
  {
    Fixture f;complete(f);f.memory.rows.erase("mtidentity/state0");f.memory.rows.erase("mtidentity/state1");
    MonocypherIdentityDeriver deriver;IdentityJournal identity;Key other(139);
    assert(!identity.load(f.memory,deriver));assert(!identity.store(f.memory,deriver,other.candidate(),true));
    PkiProvisioning observer(f.boot,f.context,f.entropy);ProvisionStatus status;
    assert(observer.inspect(status)==-EXDEV&&status.stored==ProvisionStored::Mismatch&&status.error==-EXDEV&&!status.hasBinding&&!status.hasPeer);
  }
  for(unsigned mode=0;mode<3;++mode){
    Fixture f;complete(f);PkiLedger ledger;assert(!ledger.load(f.memory));Key other(153);
    if(mode==0)assert(!ledger.addTrustedPeer(f.memory,other.binding.node,other.binding.publicKey,true));
    if(mode==1)assert(!ledger.revokeTrustedPeer(f.memory,f.peer.binding.node,f.peer.binding.publicKey,true));
    if(mode==2){assert(!ledger.revokeTrustedPeer(f.memory,f.peer.binding.node,f.peer.binding.publicKey,true));
      assert(!ledger.addTrustedPeer(f.memory,other.binding.node,other.binding.publicKey,true));}
    PkiProvisioning observer(f.boot,f.context,f.entropy);ProvisionStatus status;
    assert(!observer.inspect(status)&&status.stored==ProvisionStored::Partial&&!status.hasBinding&&!status.hasPeer);
  }
  pass("diagnostics-preserve-denied-output-and-classify-corrupt-partial-mismatch-ambiguous-revoked");
  {Fixture a(19),b(71);PkiProvisioning left(a.boot,a.context,a.entropy),right(b.boot,b.context,b.entropy);
   IdentityBinding l,r;assert(!left.stage(l)&&!right.stage(r));ProvisionPeer lp,rp;
   lp.node=l.node;memcpy(lp.publicKey,l.publicKey,32);rp.node=r.node;memcpy(rp.publicKey,r.publicKey,32);
   assert(!left.commit(l,rp));b.memory.at=2;b.memory.fault=2;assert(right.commit(r,lp)<0);
   assert(left.stage(l)<0&&right.stage(r)<0&&left.commit(l,rp)<0&&right.commit(r,lp)<0);
   assert(a.entropy.calls==1&&b.entropy.calls==1);restart(a,true);restart(b);}
  pass("two-candidates-first-one-sided-pair-failure-never-rolls-back-or-regenerates");
}
