// SPDX-License-Identifier: GPL-3.0-only
// M154 read-only reconstruction tests; synthetic settings/NOR and public keys only.
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

namespace {
struct Key {
  uint8_t scalar[32]{};
  uint8_t pub[32]{};
  uint32_t node=0;
  explicit Key(unsigned seed){
    for(unsigned i=0;i<sizeof(scalar);++i)scalar[i]=uint8_t(seed+17u*i);
    scalar[0]&=248;scalar[31]=uint8_t((scalar[31]&127)|64);
    crypto_x25519_public_key(pub,scalar);node=bench::MtSignature::nodeId(pub);
  }
  ~Key(){crypto_wipe(scalar,sizeof(scalar));}
};
struct Settings final:mt::SettingsBackend {
  std::map<std::string,Bytes> rows;unsigned reads=0,writes=0;
  int read(const char *name,uint8_t *out,size_t cap)override{
    ++reads;
    const auto it=rows.find(name);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *name,const uint8_t *data,size_t size)override{
    ++writes;rows[name]=Bytes(data,data+size);return 0;
  }
};
struct Nor final:AppendStore {
  Bytes bytes=Bytes(PkiAppendHistory::StoreBytes,0xff);
  unsigned programs=0,reads=0;size_t failAt=SIZE_MAX,reportedCapacity=SIZE_MAX;bool returnShort=false;
  size_t capacity()const override{return reportedCapacity==SIZE_MAX?bytes.size():reportedCapacity;}
  int read(size_t offset,uint8_t *out,size_t size)override{
    ++reads;
    if(!out||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    if(failAt>=offset&&failAt<offset+size)return returnShort?1:-EIO;
    memcpy(out,bytes.data()+offset,size);return 0;
  }
  int program(size_t offset,const uint8_t *data,size_t size)override{
    ++programs;
    if(!data||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    for(size_t i=0;i<size;++i)if((bytes[offset+i]&data[i])!=data[i])return -EIO;
    for(size_t i=0;i<size;++i)bytes[offset+i]&=data[i];
    return 0;
  }
};
struct Fixture {
  Key local{13},first{71},later{129};Settings settings;PkiLedger ledger;Nor nor;
  AppendAnchor anchor{};Bytes original;unsigned settingsBefore=0,programsBefore=0;
  Fixture(){
    assert(ledger.load(settings)==0);
    assert(ledger.initialize(settings,local.pub,local.node,true)==0);
    assert(ledger.addTrustedPeer(settings,first.node,first.pub,true)==0);
    original.resize(PkiLedger::RecordSize);
    assert(ledger.snapshotRead(0,original.data(),original.size()));
    PkiAppendHistory initial(nor);
    assert(initial.initialize(ledger,true,anchor)==0);
    assert(ledger.installAppendAnchor(settings,anchor,true)==0);
    assert(initial.open(ledger,anchor)==0);
    // Post-baseline peer lifecycle events must continue to replay after root
    // baseline reconstruction; they are never compacted away by this API.
    assert(initial.addTrustedPeer(later.node,later.pub,true)==0);
    assert(initial.revokeTrustedPeer(first.node,first.pub,true)==0);
    settingsBefore=settings.writes;programsBefore=nor.programs;
  }
};
void reconstruct(PkiAppendHistory &history,const Bytes &expected){
  Bytes output(expected.size(),0xa5);size_t offset=0;
  while(offset<output.size()){
    const size_t n=output.size()-offset>64?64:output.size()-offset;
    const int rc=history.readBaselineChunk(offset,output.data()+offset,n);
    assert(rc==0);offset+=n;
  }
  assert(output==expected);
}
}

int main(){
  static_assert(PkiLedger::RecordSize==1944,"complete legacy snapshot");
  {
    Fixture f;Bytes reconstructed(PkiLedger::RecordSize,0xa5);AppendAnchor anchor{91,92,93,94};
    const unsigned reads=f.nor.reads,programs=f.nor.programs,settingsReads=f.settings.reads,settingsWrites=f.settings.writes;
    assert(PkiAppendHistory::reconstructBaseline(f.nor,f.local.node,f.local.pub,reconstructed.data(),anchor)==0);
    assert(reconstructed==f.original&&anchor.generation==f.anchor.generation&&anchor.snapshotCrc==f.anchor.snapshotCrc&&
           anchor.rootCrc==f.anchor.rootCrc&&anchor.capacity==f.anchor.capacity);
    assert(f.nor.reads>reads&&f.nor.programs==programs&&f.settings.reads==settingsReads&&f.settings.writes==settingsWrites);
  }
  {
    Fixture f;Bytes reconstructed(PkiLedger::RecordSize,0x3a);AppendAnchor anchor{};
    assert(PkiAppendHistory::reconstructBaseline(f.nor,f.local.node,f.local.pub,reconstructed.data(),anchor)==0);
    PkiLedger fresh;assert(fresh.loadValidatedAppendBaseline(reconstructed.data(),reconstructed.size(),anchor,f.local.node,f.local.pub)==0);
    const unsigned settingsReads=f.settings.reads,settingsWrites=f.settings.writes;
    assert(fresh.reserve(f.settings,0x12345678,true)==-EACCES);
    assert(fresh.addTrustedPeer(f.settings,f.later.node,f.later.pub,true)==-EACCES);
    assert(fresh.revokeTrustedPeer(f.settings,f.first.node,f.first.pub,true)==-EACCES);
    assert(fresh.commitAuthenticated(f.settings,f.first.node,f.first.pub,0x23456789,true)==-EACCES);
    assert(f.settings.reads==settingsReads&&f.settings.writes==settingsWrites);
    PkiAppendHistory replay(f.nor);assert(replay.open(fresh,anchor)==0);
    uint32_t active[4]={};size_t count=99;assert(replay.activePeers(active,4,count));
    assert(count==1&&active[0]==f.later.node&&replay.revoked(f.first.node));
    assert(f.settings.reads==settingsReads&&f.settings.writes==settingsWrites&&f.nor.programs==f.programsBefore);
  }
  {
    Fixture f;Bytes reconstructed(PkiLedger::RecordSize,0x4b);AppendAnchor anchor{};
    assert(PkiAppendHistory::reconstructBaseline(f.nor,f.local.node,f.local.pub,reconstructed.data(),anchor)==0);
    AppendAnchor wrong=anchor;wrong.rootCrc^=1;
    PkiLedger fresh;assert(fresh.loadValidatedAppendBaseline(reconstructed.data(),reconstructed.size(),wrong,f.local.node,f.local.pub)==0);
    PkiAppendHistory replay(f.nor);assert(replay.open(fresh,wrong)==-EILSEQ&&!replay.ready());
    assert(f.nor.programs==f.programsBefore&&f.settings.writes==f.settingsBefore);
  }
  {
    Fixture f;Key wrong{237};Bytes output(PkiLedger::RecordSize,0x5a);const Bytes original=output;
    AppendAnchor anchor{11,12,13,14},originalAnchor=anchor;
    assert(PkiAppendHistory::reconstructBaseline(f.nor,wrong.node,wrong.pub,output.data(),anchor)==-EILSEQ);
    assert(output==original&&anchor.generation==originalAnchor.generation&&anchor.snapshotCrc==originalAnchor.snapshotCrc&&
           anchor.rootCrc==originalAnchor.rootCrc&&anchor.capacity==originalAnchor.capacity);
  }
  {
    Fixture f;Bytes output(PkiLedger::RecordSize,0x6c);const Bytes original=output;AppendAnchor anchor{21,22,23,24},originalAnchor=anchor;
    f.nor.failAt=12;f.nor.returnShort=false;
    assert(PkiAppendHistory::reconstructBaseline(f.nor,f.local.node,f.local.pub,output.data(),anchor)==-EIO);
    assert(output==original&&anchor.generation==originalAnchor.generation&&anchor.rootCrc==originalAnchor.rootCrc&&f.nor.programs==f.programsBefore);
  }
  {
    Fixture f;Bytes output(PkiLedger::RecordSize,0x7d);const Bytes original=output;AppendAnchor anchor{31,32,33,34},originalAnchor=anchor;
    f.nor.failAt=96;f.nor.returnShort=true;
    assert(PkiAppendHistory::reconstructBaseline(f.nor,f.local.node,f.local.pub,output.data(),anchor)==-EIO);
    assert(output==original&&anchor.generation==originalAnchor.generation&&anchor.rootCrc==originalAnchor.rootCrc&&f.nor.programs==f.programsBefore);
  }
  {
    Fixture f;Nor damaged=f.nor;damaged.bytes[128+20]^=1;Bytes output(PkiLedger::RecordSize,0x8e);const Bytes original=output;
    AppendAnchor anchor{41,42,43,44},originalAnchor=anchor;
    assert(PkiAppendHistory::reconstructBaseline(damaged,f.local.node,f.local.pub,output.data(),anchor)==-EILSEQ);
    assert(output==original&&anchor.generation==originalAnchor.generation&&anchor.rootCrc==originalAnchor.rootCrc&&damaged.programs==f.programsBefore);
  }
  {
    Fixture f;Nor shortStore=f.nor;shortStore.reportedCapacity--;const unsigned reads=shortStore.reads;
    Bytes output(PkiLedger::RecordSize,0x9f);const Bytes original=output;AppendAnchor anchor{51,52,53,54},originalAnchor=anchor;
    assert(PkiAppendHistory::reconstructBaseline(shortStore,f.local.node,f.local.pub,output.data(),anchor)==-EXDEV);
    assert(output==original&&anchor.generation==originalAnchor.generation&&anchor.rootCrc==originalAnchor.rootCrc&&shortStore.reads==reads);
  }
  {
    Fixture f;PkiAppendHistory replay(f.nor);assert(replay.open(f.ledger,f.anchor)==0);
    const unsigned reads=f.nor.reads,writes=f.nor.programs,settingsWrites=f.settings.writes;
    reconstruct(replay,f.original);
    assert(f.nor.reads>reads&&f.nor.programs==writes&&f.settings.writes==settingsWrites);
    uint32_t active[4]={};size_t count=99;assert(replay.activePeers(active,4,count));
    assert(count==1&&active[0]==f.later.node);
    assert(replay.revoked(f.first.node));
    assert(f.nor.bytes.size()==PkiAppendHistory::StoreBytes);
  }
  {
    Fixture f;PkiAppendHistory unopened(f.nor);uint8_t out[8];memset(out,0x5a,sizeof(out));
    assert(unopened.readBaselineChunk(0,out,sizeof(out))==-EACCES);
    for(uint8_t value:out)assert(value==0x5a);
    PkiAppendHistory opened(f.nor);assert(opened.open(f.ledger,f.anchor)==0);
    assert(opened.readBaselineChunk(PkiLedger::RecordSize-1,out,2)==-EINVAL);
    for(uint8_t value:out)assert(value==0x5a);
  }
  {
    Fixture f;PkiAppendHistory replay(f.nor);assert(replay.open(f.ledger,f.anchor)==0);
    uint8_t out[32];memset(out,0x6b,sizeof(out));
    f.nor.failAt=128+24; // Root-backed baseline read.
    assert(replay.readBaselineChunk(0,out,sizeof(out))<0);
    for(uint8_t value:out)assert(value==0x6b);
    assert(f.nor.programs==f.programsBefore&&f.settings.writes==f.settingsBefore);
  }
  {
    Fixture f;PkiAppendHistory replay(f.nor);assert(replay.open(f.ledger,f.anchor)==0);
    uint8_t out[32];memset(out,0x3c,sizeof(out));
    f.nor.failAt=128+64;f.nor.returnShort=true;
    assert(replay.readBaselineChunk(64,out,sizeof(out))<0);
    for(uint8_t value:out)assert(value==0x3c);
    assert(f.nor.programs==f.programsBefore&&f.settings.writes==f.settingsBefore);
  }
  {
    Fixture f;PkiAppendHistory replay(f.nor);assert(replay.open(f.ledger,f.anchor)==0);
    uint8_t out[32];memset(out,0xc3,sizeof(out));
    f.nor.bytes[128+7]^=1;
    assert(replay.readBaselineChunk(0,out,sizeof(out))==-EILSEQ);
    for(uint8_t value:out)assert(value==0xc3);
    assert(f.nor.programs==f.programsBefore&&f.settings.writes==f.settingsBefore);
  }
  {
    Fixture f;Key wrongLocal{211};Settings wrongSettings;PkiLedger wrongLedger;
    assert(wrongLedger.load(wrongSettings)==0);
    assert(wrongLedger.initialize(wrongSettings,wrongLocal.pub,wrongLocal.node,true)==0);
    PkiAppendHistory wrongRoot(f.nor);assert(wrongRoot.open(wrongLedger,f.anchor)<0);
    uint8_t out[32];memset(out,0x91,sizeof(out));
    assert(wrongRoot.readBaselineChunk(0,out,sizeof(out))==-EACCES);
    for(uint8_t value:out)assert(value==0x91);
    assert(f.nor.programs==f.programsBefore&&f.settings.writes==f.settingsBefore);
  }
  puts("PASS_M154_READONLY_APPEND_BASELINE_RECONSTRUCTION");
  return 0;
}
