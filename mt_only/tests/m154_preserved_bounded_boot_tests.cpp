// SPDX-License-Identifier: GPL-3.0-only
// M154 bounded startup from preserved root; synthetic storage only.
#include "pki/PkiStorage.h"
#include "pki/PkiAppendHistory.h"
#include "MtSignature.h"
#include "monocypher.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
using Values=std::map<std::string,Bytes>;

namespace {
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding{};
  explicit Key(unsigned seed){
    for(unsigned i=0;i<sizeof(scalar);++i)scalar[i]=uint8_t(seed+23u*i);
    scalar[0]&=248;scalar[31]=uint8_t((scalar[31]&127)|64);
    crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;
  }
  ~Key(){crypto_wipe(scalar,sizeof(scalar));}
};
struct Settings final:mt::SettingsBackend {
  Values rows;unsigned reads=0,writes=0;
  int read(const char *name,uint8_t *out,size_t cap)override{
    ++reads;const auto it=rows.find(name);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;rows[name]=Bytes(data,data+size);return 0;
  }
};
struct Boot final:BootSettings {
  Settings &settings;bool held=false;unsigned begins=0,ends=0;
  explicit Boot(Settings &s):settings(s){}
  int begin()override{if(held)return -EBUSY;held=true;++begins;return 0;}
  void end()override{assert(held);held=false;++ends;}
  int read(const char *name,uint8_t *out,size_t size)override{return held?settings.read(name,out,size):-EACCES;}
};
struct Context final:PkiStorageContext {
  unsigned depth=0;bool radioReady=true,storagePaused=false;
  bool tryEnter()override{if(depth)return false;depth=1;return true;}
  void leave()override{assert(depth==1);depth=0;}
  bool bootQuiescent()const override{return depth==1&&radioReady;}
  bool quiescent()const override{return depth==1&&storagePaused;}
};
struct Entropy final:EntropySource {bool fill(uint8_t *out,size_t n)override{if(!out)return false;memset(out,0x61,n);return n==4;}};
struct Nor final:AppendStore {
  Bytes bytes=Bytes(PkiAppendHistory::StoreBytes,0xff);unsigned reads=0,programs=0;
  size_t capacity()const override{return bytes.size();}
  int read(size_t off,uint8_t *out,size_t n)override{
    ++reads;if(!out||off>bytes.size()||n>bytes.size()-off)return -EINVAL;memcpy(out,bytes.data()+off,n);return 0;
  }
  int program(size_t off,const uint8_t *data,size_t n)override{
    ++programs;if(!data||off>bytes.size()||n>bytes.size()-off)return -EINVAL;
    for(size_t i=0;i<n;++i)if((bytes[off+i]&data[i])!=data[i])return -EIO;
    for(size_t i=0;i<n;++i)bytes[off+i]&=data[i];return 0;
  }
  int selectCounterMode()override{
    const uint8_t bmit[4]={0x42,0x4d,0x49,0x54};if(memcmp(bytes.data()+4092,"CMIT",4))return -EILSEQ;
    return program(4092,bmit,sizeof(bmit));
  }
};
struct CounterSettings final:PkiNonceCounterStore {
  Settings &settings;explicit CounterSettings(Settings &s):settings(s){}
  int read(unsigned slot,uint8_t *out,size_t n)override{
    return slot<2?settings.read(slot?"mtpki/nonces1":"mtpki/nonces0",out,n):-EINVAL;
  }
  int write(unsigned slot,const uint8_t *data,size_t n)override{
    return slot<2?settings.write(slot?"mtpki/nonces1":"mtpki/nonces0",data,n):-EINVAL;
  }
};
struct Durable {
  Key local{29},first{83},later{171};Settings settings;MonocypherIdentityDeriver deriver;
  IdentityJournal identity;PkiLedger ledger;Nor nor;AppendAnchor anchor{};
  unsigned setupSettingsWrites=0,setupNorPrograms=0;
  Durable(){
    assert(identity.load(settings,deriver)==0);IdentityCandidate candidate(local.binding,local.scalar);
    assert(identity.store(settings,deriver,candidate,true)==0);
    assert(ledger.load(settings)==0&&ledger.initialize(settings,local.binding.publicKey,local.binding.node,true)==0);
    assert(ledger.addTrustedPeer(settings,first.binding.node,first.binding.publicKey,true)==0);
    PkiAppendHistory initial(nor);assert(initial.initialize(ledger,true,anchor)==0);
    assert(ledger.installAppendAnchor(settings,anchor,true)==0&&initial.open(ledger,anchor)==0);
    assert(initial.commitAuthenticated(first.binding.node,first.binding.publicKey,0x7a110001u,true)==0);
    assert(initial.addTrustedPeer(later.binding.node,later.binding.publicKey,true)==0);
    assert(initial.revokeTrustedPeer(first.binding.node,first.binding.publicKey,true)==0);
    for(uint32_t i=0;i<PkiAppendHistory::EventLimit-3;++i)
      assert(initial.reserve(0x51000001u+i,true)==0);
    assert(initial.remaining()==0);PkiLedger::Status status;assert(initial.status(status));
    PkiNonceCounterBinding binding{};binding.node=local.binding.node;
    memcpy(binding.publicKey,local.binding.publicKey,32);binding.legacyGeneration=status.generation;
    binding.legacyCrc=anchor.rootCrc;CounterSettings counterStore(settings);PkiNonceCounter counter(counterStore);
    assert(counter.initialize(binding,true)==0&&initial.activateCounterMode(true)==0&&initial.counterMode());
    uint32_t id=0;assert(counter.open(binding,true)==0&&counter.reserve(id)==0&&id==1);counter.close();
    setupSettingsWrites=settings.writes;setupNorPrograms=nor.programs;
  }
};
struct Attempt {
  Settings settings;Boot boot{settings};Context context;Entropy entropy;Nor nor;PkiAppendHistory append{nor};
  PkiStorage storage{boot,settings,context,entropy,&append};
  explicit Attempt(const Values &rows,const Bytes &flash):settings(){settings.rows=rows;nor.bytes=flash;}
};
void assertNotPrepared(PkiStorage &storage){
  assert(storage.state()!=PkiStorage::State::Prepared&&storage.state()!=PkiStorage::State::Active);
  assert(!storage.boundedHistory());assert(storage.activate(0)<0);
}
void failClosed(const Values &rows,const Bytes &flash,int expected=-1){
  Attempt attempt(rows,flash);const Values before=attempt.settings.rows;const unsigned writes=attempt.settings.writes;
  const unsigned programs=attempt.nor.programs;const int rc=attempt.storage.prepare();
  assert(rc<0&&(expected==-1||rc==expected));assertNotPrepared(attempt.storage);
  assert(attempt.settings.rows==before&&attempt.settings.writes==writes&&attempt.nor.programs==programs);
  assert(!attempt.boot.held&&attempt.boot.begins==attempt.boot.ends);
}
}

int main(){
  static_assert(PkiAppendHistory::EventLimit==2624,"full preserved event history");
  Durable durable;
  {
    Values nvs=durable.settings.rows;
    Attempt attempt(nvs,durable.nor.bytes);const Values before=attempt.settings.rows;
    const unsigned writes=attempt.settings.writes,programs=attempt.nor.programs;
    assert(attempt.storage.prepare()==0&&attempt.storage.state()==PkiStorage::State::Prepared);
    PkiPeerSet peers;assert(attempt.storage.preparedPeers(peers));
    assert(peers.count()==1&&peers.find(durable.later.binding.node));
    assert(!peers.find(durable.first.binding.node));
    assert(attempt.settings.rows==before&&attempt.settings.writes==writes&&attempt.nor.programs==programs);
    assert(attempt.storage.activate(durable.local.binding.node)==0);
    assert(attempt.storage.extendedStorage()&&attempt.storage.boundedHistory());
    assert(attempt.storage.nonceReservedThrough()==PkiNonceCounter::DefaultBlockSize);
    KeyRef ref;IdentityBinding current;IdentityJournal *identity=attempt.storage.runtimeIdentity();
    assert(identity&&identity->binding(current,ref)&&ref.generation==durable.local.binding.generation);
    attempt.context.storagePaused=true;uint32_t extra=0;
    assert(attempt.storage.reserve(ref,current.publicKey,current.node,0x60000001u,extra));
    assert(extra==PkiNonceCounter::DefaultBlockSize+1&&
           attempt.storage.nonceReservedThrough()==2*PkiNonceCounter::DefaultBlockSize);
    assert(attempt.nor.programs==programs);
  }
  {
    Values corrupt=durable.settings.rows;
    for(const char *key: {"mtpki/ledger0","mtpki/ledger1"}){
      auto it=corrupt.find(key);if(it!=corrupt.end()&&!it->second.empty())it->second[100]^=0x40;
    }
    // Full contact-layout admission follows the cold root proof. Corrupt or
    // missing compact-state slots cannot resurrect the original trust set.
    failClosed(corrupt,durable.nor.bytes);
  }
  {
    Values missing=durable.settings.rows;missing.erase("mtpki/ledger0");missing.erase("mtpki/ledger1");
    failClosed(missing,durable.nor.bytes);
  }
  {
    Values missing=durable.settings.rows;missing.erase("mtpki/nonces1");failClosed(missing,durable.nor.bytes);
  }
  {
    Values bad=durable.settings.rows;bad["mtpki/nonces0"][0]^=0x80;failClosed(bad,durable.nor.bytes);
  }
  {
    Bytes oldSelector=durable.nor.bytes;oldSelector[4092]=0x43;failClosed(durable.settings.rows,oldSelector,-EILSEQ);
  }
  {
    Bytes corrupt=durable.nor.bytes;corrupt[128+32]^=1;failClosed(durable.settings.rows,corrupt);
  }
  {
    Values wrongIdentity=durable.settings.rows;
    // A CRC-valid other identity is built independently, then substituted only
    // into the identity rows; the preserved legacy root remains unchanged.
    Key other{239};Settings otherSettings;MonocypherIdentityDeriver deriver;IdentityJournal journal;
    assert(journal.load(otherSettings,deriver)==0);IdentityCandidate candidate(other.binding,other.scalar);
    assert(journal.store(otherSettings,deriver,candidate,true)==0);
    for(const char *key: {"mtidentity/state0","mtidentity/state1"}){
      wrongIdentity.erase(key);const auto it=otherSettings.rows.find(key);
      if(it!=otherSettings.rows.end())wrongIdentity[key]=it->second;
    }
    failClosed(wrongIdentity,durable.nor.bytes);
  }
  {
    Attempt noAppend(durable.settings.rows,durable.nor.bytes);
    // Exercise the production no-append policy without changing the shared fixture.
    PkiStorage noRoot(noAppend.boot,noAppend.settings,noAppend.context,noAppend.entropy,nullptr);
    const Values before=noAppend.settings.rows;const unsigned writes=noAppend.settings.writes;
    assert(noRoot.prepare()<0);assertNotPrepared(noRoot);
    assert(noAppend.settings.rows==before&&noAppend.settings.writes==writes);
  }
  puts("PASS_M154_PRESERVED_ROOT_BOUNDED_BOOT");return 0;
}
