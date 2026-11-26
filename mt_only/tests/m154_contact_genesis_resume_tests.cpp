// SPDX-License-Identifier: GPL-3.0-only
// Explicit interrupted-contact-genesis resume tests; synthetic settings only.
#include "pki/PkiContactStore.h"
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
using Rows=std::map<std::string,Bytes>;

namespace {
constexpr uint32_t RootGeneration=77,RootCrc=0x91ac52e3;
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding{};
  explicit Key(unsigned seed){
    for(unsigned i=0;i<sizeof(scalar);++i)scalar[i]=uint8_t(seed+29u*i);
    scalar[0]&=248;scalar[31]=uint8_t((scalar[31]&127)|64);
    crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;
  }
  ~Key(){crypto_wipe(scalar,sizeof(scalar));}
};
struct Memory final:mt::SettingsBackend {
  Rows rows;unsigned reads=0,writes=0;std::string failReadKey;
  int failReadAt=-1,failWriteAt=-1;bool commitOnWriteError=false,tearOnWriteError=false;
  int read(const char *name,uint8_t *out,size_t cap)override{
    ++reads;if(int(reads)==failReadAt||failReadKey==name)return -EIO;
    const auto it=rows.find(name);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;if(int(writes)==failWriteAt){
      if(commitOnWriteError)rows[name]=Bytes(data,data+size);
      else if(tearOnWriteError)rows[name]=Bytes(data,data+size/2);
      return -EIO;
    }
    rows[name]=Bytes(data,data+size);return 0;
  }
};
struct Case {
  Key local{17},a{81},b{159},wrong{221};PkiPeerPin expected[2]{};
  Case(){
    expected[0].node=a.binding.node;memcpy(expected[0].publicKey,a.binding.publicKey,32);
    expected[1].node=b.binding.node;memcpy(expected[1].publicKey,b.binding.publicKey,32);
  }
  Bytes record(uint32_t generation,const PkiPeerPin *pins=nullptr,size_t count=2,
               const IdentityBinding *identity=nullptr,uint32_t rootGen=RootGeneration,
               uint32_t rootCrc=RootCrc)const{
    if(!pins)pins=expected;
    Bytes bytes(PkiContactSnapshot::RecordBytes);
    assert(PkiContactSnapshot::encode(identity?*identity:local.binding,generation,rootGen,rootCrc,
                                       pins,count,bytes.data(),bytes.size()));return bytes;
  }
  Memory interrupted()const{Memory mem;mem.rows[PkiContactStore::key(0)]=record(1);return mem;}
};
void expectNotReady(const PkiContactStore &store){
  assert(!store.ready());PkiContactSnapshot::Value out{};out.generation=0x12345678;
  assert(!store.snapshot(out)&&out.generation==0x12345678);
}
void recoverExactGenesis(){
  Case c;Memory mem=c.interrupted();PkiContactStore store;
  assert(store.load(mem,c.local.binding,RootGeneration,RootCrc)==-EILSEQ);
  expectNotReady(store);assert(mem.writes==0);
  assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)==0);
  assert(store.ready()&&store.activeSlot()==1&&store.generation()==2&&mem.writes==1);
  PkiContactSnapshot::Value value{};assert(store.snapshot(value)&&value.generation==2&&value.count==2);
  PkiContactStore reopened;assert(reopened.load(mem,c.local.binding,RootGeneration,RootCrc)==0);
  assert(reopened.ready()&&reopened.generation()==2&&mem.writes==1);
}
void noGenesisFromEmptyOrWrongExpected(){
  Case c;
  {
    Memory empty;PkiContactStore store;assert(store.resumeGenesis(empty,c.local.binding,RootGeneration,RootCrc,c.expected,2)==-ENODATA);
    expectNotReady(store);assert(empty.rows.empty()&&empty.writes==0);
  }
  {
    Memory mem=c.interrupted();PkiContactStore store;assert(store.load(mem,c.local.binding,RootGeneration,RootCrc)==-EILSEQ);
    PkiPeerPin wrong[2]={c.expected[0],{}};wrong[1].node=c.wrong.binding.node;
    memcpy(wrong[1].publicKey,c.wrong.binding.publicKey,32);
    const Rows before=mem.rows;assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,wrong,2)<0);
    expectNotReady(store);assert(mem.rows==before&&mem.writes==0);
  }
  {
    Memory mem=c.interrupted();PkiContactStore store;
    assert(store.resumeGenesis(mem,c.wrong.binding,RootGeneration,RootCrc,c.expected,2)==-EILSEQ);
    expectNotReady(store);assert(mem.writes==0);
  }
  {
    Memory mem=c.interrupted();PkiContactStore store;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc^1,c.expected,2)==-EILSEQ);
    expectNotReady(store);assert(mem.writes==0);
  }
  {
    Memory mem=c.interrupted();PkiContactStore store;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration+1,RootCrc,c.expected,2)==-EILSEQ);
    expectNotReady(store);assert(mem.writes==0);
  }
}
void refusesPresentNewerOrUnreadableSlots(){
  Case c;
  {
    Memory mem=c.interrupted();mem.rows[PkiContactStore::key(1)]=c.record(2);
    PkiContactStore store;assert(store.load(mem,c.local.binding,RootGeneration,RootCrc)==0&&store.ready());
    const Rows before=mem.rows;const unsigned writes=mem.writes;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)==-EACCES);
    assert(store.ready()&&store.generation()==2&&mem.rows==before&&mem.writes==writes);
  }
  {
    Memory mem=c.interrupted();mem.rows[PkiContactStore::key(1)]=Bytes(PkiContactStore::RecordBytes,0);
    PkiContactStore store;const Rows before=mem.rows;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)<0);
    expectNotReady(store);assert(mem.rows==before&&mem.writes==0);
  }
  {
    Memory mem=c.interrupted();mem.rows[PkiContactStore::key(1)]=c.record(3);
    PkiContactStore store;const Rows before=mem.rows;
    assert(store.load(mem,c.local.binding,RootGeneration,RootCrc)==-EILSEQ);
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)<0);
    expectNotReady(store);assert(mem.rows==before&&mem.writes==0);
  }
  {
    Memory mem=c.interrupted();mem.rows[PkiContactStore::key(0)]=c.record(3);
    PkiContactStore store;const Rows before=mem.rows;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)<0);
    expectNotReady(store);assert(mem.rows==before&&mem.writes==0);
  }
  for(unsigned slot=0;slot<2;++slot){
    Memory mem=c.interrupted();mem.failReadKey=PkiContactStore::key(slot);
    PkiContactStore store;const Rows before=mem.rows;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)==-EIO);
    expectNotReady(store);assert(mem.rows==before&&mem.writes==0);
  }
}
void uncertaintyNeverPublishes(){
  Case c;
  for(unsigned mode=0;mode<2;++mode){
    Memory mem=c.interrupted();PkiContactStore store;
    assert(store.load(mem,c.local.binding,RootGeneration,RootCrc)==-EILSEQ);
    mem.failWriteAt=1;mem.commitOnWriteError=mode==0;mem.tearOnWriteError=mode==1;
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)==-EIO);
    assert(store.state()==PkiContactStore::State::Frozen);expectNotReady(store);
    assert(mem.writes==1);
  }
  {
    Memory mem=c.interrupted();PkiContactStore store;
    assert(store.load(mem,c.local.binding,RootGeneration,RootCrc)==-EILSEQ);
    mem.failReadAt=int(mem.reads+3); // Fail the generation-2 write readback.
    assert(store.resumeGenesis(mem,c.local.binding,RootGeneration,RootCrc,c.expected,2)==-EIO);
    assert(store.state()==PkiContactStore::State::Frozen);expectNotReady(store);
    assert(mem.writes==1&&mem.rows.count(PkiContactStore::key(1))==1);
  }
}
}

int main(){
  recoverExactGenesis();noGenesisFromEmptyOrWrongExpected();
  refusesPresentNewerOrUnreadableSlots();uncertaintyNeverPublishes();
  puts("PASS_M154_CONTACT_GENESIS_EXPLICIT_RESUME");return 0;
}
