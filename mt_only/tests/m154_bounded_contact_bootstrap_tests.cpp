// SPDX-License-Identifier: GPL-3.0-only
// M154 private BMIT/history/nonce cold-boot composition tests.
#include "pki/PkiBoundedContactBootstrap.h"
#include "MtSignature.h"
#include "monocypher.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

using namespace mt::pki;
using Bytes=std::vector<uint8_t>;

namespace {
struct Key {
  uint8_t scalar[32]{};uint8_t pub[32]{};IdentityBinding binding{};uint32_t node=0;
  explicit Key(unsigned seed){
    for(unsigned i=0;i<sizeof(scalar);++i)scalar[i]=uint8_t(seed+17u*i);
    scalar[0]&=248;scalar[31]=uint8_t((scalar[31]&127)|64);
    crypto_x25519_public_key(pub,scalar);node=bench::MtSignature::nodeId(pub);
    binding.generation=1;binding.node=node;memcpy(binding.publicKey,pub,sizeof(pub));
  }
  ~Key(){crypto_wipe(scalar,sizeof(scalar));}
};
struct Settings final:mt::SettingsBackend {
  std::map<std::string,Bytes> rows;unsigned reads=0,writes=0;
  int read(const char *name,uint8_t *out,size_t cap)override{
    ++reads;const auto it=rows.find(name);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;rows[name]=Bytes(data,data+size);return 0;
  }
};
struct Nor final:AppendStore {
  Bytes bytes=Bytes(PkiAppendHistory::StoreBytes,0xff);unsigned reads=0,programs=0;
  size_t failAt=SIZE_MAX;bool shortRead=false;
  size_t capacity()const override{return bytes.size();}
  int read(size_t offset,uint8_t *out,size_t size)override{
    ++reads;if(!out||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    if(failAt>=offset&&failAt<offset+size)return shortRead?1:-EIO;
    memcpy(out,bytes.data()+offset,size);return 0;
  }
  int program(size_t offset,const uint8_t *data,size_t size)override{
    ++programs;if(!data||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    for(size_t i=0;i<size;++i)if((bytes[offset+i]&data[i])!=data[i])return -EIO;
    for(size_t i=0;i<size;++i)bytes[offset+i]&=data[i];return 0;
  }
  int selectCounterMode()override{
    const uint8_t commit[4]={0x42,0x4d,0x49,0x54};
    if(memcmp(bytes.data()+4092,"CMIT",4))return -EILSEQ;
    return program(4092,commit,sizeof(commit));
  }
};
struct CounterMemory final:PkiNonceCounterStore {
  Bytes slots[2];unsigned reads=0,writes=0;
  int read(unsigned slot,uint8_t *out,size_t size)override{
    ++reads;if(slot>=2||!out||size!=PkiNonceCounter::RecordBytes)return -EINVAL;
    if(slots[slot].empty())return -ENOENT;
    if(slots[slot].size()!=size)return -EILSEQ;
    memcpy(out,slots[slot].data(),size);return int(size);
  }
  int write(unsigned slot,const uint8_t *data,size_t size)override{
    ++writes;if(slot>=2||!data||size!=PkiNonceCounter::RecordBytes)return -EINVAL;
    slots[slot]=Bytes(data,data+size);return 0;
  }
};
struct Durable {
  Key local{29},first{79},later{157};Settings settings;MonocypherIdentityDeriver deriver;
  IdentityJournal identity;PkiLedger ledger;Nor nor;CounterMemory counters;AppendAnchor anchor{};
  unsigned settingsReads=0,settingsWrites=0,norPrograms=0,counterWrites=0;
  Durable(){
    assert(identity.load(settings,deriver)==0);
    IdentityCandidate candidate(local.binding,local.scalar);assert(identity.store(settings,deriver,candidate,true)==0);
    assert(ledger.load(settings)==0);assert(ledger.initialize(settings,local.pub,local.node,true)==0);
    assert(ledger.addTrustedPeer(settings,first.node,first.pub,true)==0);
    PkiAppendHistory initial(nor);assert(initial.initialize(ledger,true,anchor)==0);
    assert(ledger.installAppendAnchor(settings,anchor,true)==0);assert(initial.open(ledger,anchor)==0);
    assert(initial.addTrustedPeer(later.node,later.pub,true)==0);
    assert(initial.revokeTrustedPeer(first.node,first.pub,true)==0);
    for(uint32_t i=0;i<PkiAppendHistory::EventLimit-2;++i)
      assert(initial.reserve(0x40000001u+i,true)==0);
    assert(initial.remaining()==0);
    PkiLedger::Status status;assert(initial.status(status));
    PkiNonceCounterBinding binding{};binding.node=local.node;memcpy(binding.publicKey,local.pub,32);
    binding.legacyGeneration=status.generation;binding.legacyCrc=anchor.rootCrc;
    PkiNonceCounter seed(counters);assert(seed.initialize(binding,true)==0);
    assert(initial.activateCounterMode(true)==0&&initial.counterMode());
    uint32_t firstNonce=0;assert(seed.open(binding,true)==0&&seed.reserve(firstNonce)==0&&firstNonce==1);
    seed.close();
    settingsReads=settings.reads;settingsWrites=settings.writes;norPrograms=nor.programs;counterWrites=counters.writes;
  }
};
void unavailable(const PkiBoundedContactBootstrap &boot){
  assert(!boot.ready());PkiLedger::Status status;status.generation=0x12345678;
  assert(!boot.status(status)&&status.generation==0x12345678);
  uint32_t nodes[4]={1,2,3,4};size_t count=77;
  assert(!boot.activePeers(nodes,4,count)&&count==77&&nodes[0]==1);
  uint8_t pub[32];memset(pub,0x5a,sizeof(pub));assert(!boot.lookup(1,pub));
  for(uint8_t b:pub)assert(b==0x5a);
  assert(boot.nonceReservedThrough()==0);
}
}

int main(){
  static_assert(PkiAppendHistory::EventLimit==2624,"complete BMIT history");
  // Build one expensive full committed journal. Every negative branch below
  // receives an independent copy of the immutable backing bytes, so test cost
  // is dominated by one real 2,624-event setup rather than repeated setup.
  Durable d;
  {
    PkiBoundedContactBootstrap boot(d.nor,d.counters,d.identity);unavailable(boot);
    const unsigned settingReads=d.settings.reads,settingWrites=d.settings.writes;
    const unsigned norReads=d.nor.reads,norPrograms=d.nor.programs,counterReads=d.counters.reads,counterWrites=d.counters.writes;
    assert(boot.prepare()==0&&boot.ready());
    PkiLedger::Status status;assert(boot.status(status));
    assert(status.format==3&&status.generation==d.anchor.generation+PkiAppendHistory::EventLimit&&
           status.active==1&&status.stored==2&&boot.nonceReservedThrough()==PkiNonceCounter::DefaultBlockSize);
    uint32_t nodes[4]={};size_t count=0;assert(boot.activePeers(nodes,4,count));
    assert(count==1&&nodes[0]==d.later.node&&boot.lookup(d.later.node,d.later.pub)&&!boot.lookup(d.first.node,d.first.pub));
    assert(d.settings.reads==settingReads&&d.settings.writes==settingWrites&&d.nor.reads>norReads&&
           d.nor.programs==norPrograms&&d.counters.reads>counterReads&&d.counters.writes==counterWrites);
    assert(boot.prepare()==-EACCES);
  }
  {
    CounterMemory wrong=d.counters;wrong.slots[0][24]^=0x80;
    PkiBoundedContactBootstrap boot(d.nor,wrong,d.identity);unavailable(boot);
    const unsigned writes=wrong.writes,programs=d.nor.programs;
    assert(boot.prepare()<0&&boot.state()==PkiBoundedContactBootstrap::State::Failed);unavailable(boot);
    assert(wrong.writes==writes&&d.nor.programs==programs);
  }
  {
    CounterMemory wrong=d.counters;wrong.slots[0][32]^=0x01;
    PkiBoundedContactBootstrap boot(d.nor,wrong,d.identity);unavailable(boot);
    const unsigned writes=wrong.writes,programs=d.nor.programs;
    assert(boot.prepare()<0);unavailable(boot);assert(wrong.writes==writes&&d.nor.programs==programs);
  }
  {
    CounterMemory wrong;PkiNonceCounterBinding binding{};binding.node=d.local.node;
    memcpy(binding.publicKey,d.local.pub,32);binding.legacyGeneration=d.anchor.generation+PkiAppendHistory::EventLimit;
    binding.legacyCrc=d.anchor.rootCrc^1;PkiNonceCounter seed(wrong);assert(seed.initialize(binding,true)==0);
    PkiBoundedContactBootstrap boot(d.nor,wrong,d.identity);unavailable(boot);
    const unsigned writes=wrong.writes,programs=d.nor.programs;
    assert(boot.prepare()==-EXDEV);unavailable(boot);assert(wrong.writes==writes&&d.nor.programs==programs);
  }
  {
    Nor torn=d.nor;torn.bytes[4092]=0xff;
    PkiBoundedContactBootstrap boot(torn,d.counters,d.identity);unavailable(boot);
    const unsigned writes=d.counters.writes,programs=torn.programs;
    assert(boot.prepare()<0);unavailable(boot);assert(d.counters.writes==writes&&torn.programs==programs);
  }
  {
    Nor corrupt=d.nor;corrupt.bytes[128+40]^=1;
    PkiBoundedContactBootstrap boot(corrupt,d.counters,d.identity);unavailable(boot);
    const unsigned reads=d.counters.reads,writes=d.counters.writes,programs=corrupt.programs;
    assert(boot.prepare()==-EILSEQ);unavailable(boot);
    assert(d.counters.reads==reads&&d.counters.writes==writes&&corrupt.programs==programs);
  }
  {
    Nor shortRead=d.nor;shortRead.failAt=96;shortRead.shortRead=true;
    PkiBoundedContactBootstrap boot(shortRead,d.counters,d.identity);unavailable(boot);
    const unsigned reads=d.counters.reads,writes=d.counters.writes,programs=shortRead.programs;
    assert(boot.prepare()==-EIO);unavailable(boot);
    assert(d.counters.reads==reads&&d.counters.writes==writes&&shortRead.programs==programs);
  }
  {
    Nor legacy=d.nor;legacy.bytes[4092]=uint8_t(0x43); // Restored old selector, full cells remain.
    PkiBoundedContactBootstrap boot(legacy,d.counters,d.identity);unavailable(boot);
    const unsigned reads=d.counters.reads,writes=d.counters.writes,programs=legacy.programs;
    assert(boot.prepare()==-EILSEQ);unavailable(boot);
    assert(d.counters.reads==reads&&d.counters.writes==writes&&legacy.programs==programs);
  }
  {
    Durable d;Settings otherSettings;MonocypherIdentityDeriver deriver;Key other{233};IdentityJournal otherIdentity;
    assert(otherIdentity.load(otherSettings,deriver)==0);IdentityCandidate candidate(other.binding,other.scalar);
    assert(otherIdentity.store(otherSettings,deriver,candidate,true)==0);
    PkiBoundedContactBootstrap boot(d.nor,d.counters,otherIdentity);unavailable(boot);
    const unsigned reads=d.counters.reads,writes=d.counters.writes,programs=d.nor.programs;
    assert(boot.prepare()==-EILSEQ);unavailable(boot);
    assert(d.counters.reads==reads&&d.counters.writes==writes&&d.nor.programs==programs);
  }
  puts("PASS_M154_BOUNDED_CONTACT_COLD_BOOT_COMPOSITION");return 0;
}
