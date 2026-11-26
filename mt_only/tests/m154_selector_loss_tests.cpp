// SPDX-License-Identifier: GPL-3.0-only
// M154 selector-loss nonce downgrade regression; synthetic NVS only.
#include "pki/PkiStorage.h"
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
  uint8_t scalar[32]{};
  IdentityBinding binding{};
  explicit Key(unsigned seed=41){
    for(unsigned i=0;i<sizeof(scalar);++i)scalar[i]=uint8_t(seed+19u*i);
    scalar[0]&=248;scalar[31]=uint8_t((scalar[31]&127)|64);
    crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);
    binding.generation=1;
  }
  ~Key(){crypto_wipe(scalar,sizeof(scalar));}
};
struct Settings final:mt::SettingsBackend {
  Values values;
  unsigned reads=0,writes=0;
  std::string failReadKey;
  int failRead=-EIO;
  int read(const char *name,uint8_t *out,size_t capacity)override{
    ++reads;
    if(!name||!out)return -EINVAL;
    if(failReadKey==name)return failRead;
    const auto it=values.find(name);if(it==values.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    if(!it->second.empty())memcpy(out,it->second.data(),it->second.size());
    return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;
    if(!name||!data||!size)return -EINVAL;
    values[name]=Bytes(data,data+size);return 0;
  }
};
struct Boot final:BootSettings {
  Settings &settings;bool held=false;unsigned begins=0,ends=0;
  explicit Boot(Settings &s):settings(s){}
  int begin()override{if(held)return -EBUSY;held=true;++begins;return 0;}
  void end()override{assert(held);held=false;++ends;}
  int read(const char *name,uint8_t *out,size_t size)override{
    return held?settings.read(name,out,size):-EACCES;
  }
};
struct Context final:PkiStorageContext {
  unsigned depth=0;bool ready=true;
  bool tryEnter()override{if(depth)return false;depth=1;return true;}
  void leave()override{assert(depth==1);depth=0;}
  bool bootQuiescent()const override{return depth==1&&ready;}
  bool quiescent()const override{return false;}
};
struct Entropy final:EntropySource {
  bool fill(uint8_t *out,size_t size)override{
    if(!out||size!=4)return false;memset(out,0x6d,size);return true;
  }
};
struct CounterSettings final:PkiNonceCounterStore {
  Settings &settings;explicit CounterSettings(Settings &s):settings(s){}
  int read(unsigned slot,uint8_t *out,size_t size)override{
    return slot<2?settings.read(slot?"mtpki/nonces1":"mtpki/nonces0",out,size):-EINVAL;
  }
  int write(unsigned slot,const uint8_t *data,size_t size)override{
    return slot<2?settings.write(slot?"mtpki/nonces1":"mtpki/nonces0",data,size):-EINVAL;
  }
};
struct LegacyImage {
  Key local{41},peer{113};Values values;
  LegacyImage(){
    Settings settings;MonocypherIdentityDeriver deriver;IdentityJournal identity;PkiLedger ledger;
    assert(identity.load(settings,deriver)==0);
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(identity.store(settings,deriver,candidate,true)==0);
    assert(ledger.load(settings)==0);
    assert(ledger.initialize(settings,local.binding.publicKey,local.binding.node,true)==0);
    assert(ledger.addTrustedPeer(settings,peer.binding.node,peer.binding.publicKey,true)==0);
    assert(identity.close()&&ledger.close());
    values=settings.values;
  }
  void addValidCounters(){
    Settings settings;settings.values=values;PkiLedger ledger;
    assert(ledger.load(settings)==0);PkiLedger::Status status{};assert(ledger.status(status));
    PkiNonceCounterBinding binding{};binding.node=local.binding.node;
    memcpy(binding.publicKey,local.binding.publicKey,sizeof(binding.publicKey));
    binding.legacyGeneration=status.generation;binding.legacyCrc=0x1a2b3c4d;
    CounterSettings counterStore(settings);PkiNonceCounter counter(counterStore);
    assert(counter.initialize(binding,true)==0);counter.close();ledger.close();
    values=settings.values;
  }
};
struct Attempt {
  Settings settings;Boot boot{settings};Context context;Entropy entropy;
  PkiStorage storage{boot,settings,context,entropy};
  explicit Attempt(const Values &values):settings(){settings.values=values;}
  int prepare(){return storage.prepare();}
};
void assertRefused(const Values &values,const std::string &readErrorKey={}){
  Attempt attempt(values);attempt.settings.failReadKey=readErrorKey;
  const Values before=attempt.settings.values;const unsigned writes=attempt.settings.writes;
  const int result=attempt.prepare();
  assert(result<0&&attempt.storage.state()==PkiStorage::State::Partial);
  assert(!attempt.storage.boundedHistory());
  assert(attempt.settings.writes==writes&&attempt.settings.values==before);
  assert(!attempt.boot.held&&attempt.boot.begins==attempt.boot.ends);
  assert(attempt.storage.activate(0)==-EACCES);
}
}

int main(){
  LegacyImage legacy;
  {
    // Preserve old nonce-free legacy installations.
    Attempt attempt(legacy.values);const unsigned writes=attempt.settings.writes;
    assert(attempt.prepare()==0&&attempt.storage.state()==PkiStorage::State::Prepared);
    assert(!attempt.storage.boundedHistory());
    assert(attempt.storage.activate(legacy.local.binding.node)==0);
    assert(attempt.storage.state()==PkiStorage::State::Active);
    assert(!attempt.storage.boundedHistory());
    assert(attempt.settings.writes==writes&&attempt.settings.values==legacy.values);
  }
  legacy.addValidCounters();
  {
    // One surviving valid M148 counter record is enough to forbid fallback.
    Values one=legacy.values;one.erase("mtpki/nonces1");assertRefused(one);
  }
  {
    // Both valid records with the ZTPA selector absent also forbid fallback.
    assertRefused(legacy.values);
  }
  {
    Values onlySecond=legacy.values;onlySecond.erase("mtpki/nonces0");assertRefused(onlySecond);
  }
  {
    Values crc=legacy.values;crc["mtpki/nonces0"][0]^=0x80;assertRefused(crc);
  }
  {
    Values shortRecord=legacy.values;shortRecord["mtpki/nonces0"].pop_back();assertRefused(shortRecord);
  }
  {
    // Slot 0 must be absent to reach and characterize a slot-1 read error.
    Values readFailure=legacy.values;readFailure.erase("mtpki/nonces0");Attempt attempt(readFailure);
    attempt.settings.failReadKey="mtpki/nonces1";const Values before=attempt.settings.values;
    const unsigned writes=attempt.settings.writes;
    assert(attempt.prepare()==-EIO&&attempt.storage.state()==PkiStorage::State::Partial);
    assert(attempt.settings.values==before&&attempt.settings.writes==writes);
    assert(!attempt.boot.held&&attempt.boot.begins==attempt.boot.ends);
  }
  {
    Values noCounters=legacy.values;noCounters.erase("mtpki/nonces0");noCounters.erase("mtpki/nonces1");
    assertRefused(noCounters,"mtpki/nonces0");
  }
  puts("PASS_M154_SELECTOR_LOSS_REQUIRES_NONCE_ABSENCE");
  return 0;
}
