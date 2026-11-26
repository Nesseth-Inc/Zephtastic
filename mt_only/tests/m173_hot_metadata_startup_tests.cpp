// SPDX-License-Identifier: GPL-3.0-only
#include "mqtt/HotNodeMetadataStartup.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {
using namespace mt::mqtt;
constexpr uint32_t Self=0x12345678,Peer=0x87654321;
struct Binding {uint32_t node=0;uint8_t publicKey[32]{};};
struct Storage {
  Binding value{};bool available=true;unsigned calls=0;
  bool preparedBinding(Binding &out){++calls;if(!available)return false;out=value;return true;}
};
struct Slot {char key[20]{};uint8_t bytes[HotNodeMetadataJournal::RecordSize]{};size_t size=0;};
struct Backend final:mt::SettingsBackend {
  Slot slots[2]{};unsigned reads=0,writes=0;int readError=0,writeError=0;
  Slot *find(const char *key){for(auto &s:slots)if(s.key[0]&&!strcmp(s.key,key))return &s;return nullptr;}
  int read(const char *key,uint8_t *out,size_t cap)override{
    ++reads;if(readError)return readError;auto *s=find(key);if(!s)return -ENOENT;
    if(s->size>cap)return -E2BIG;memcpy(out,s->bytes,s->size);return int(s->size);
  }
  int write(const char *key,const uint8_t *data,size_t n)override{
    ++writes;if(writeError)return writeError;auto *s=find(key);
    if(!s)for(auto &x:slots)if(!x.key[0]){s=&x;strncpy(x.key,key,sizeof(x.key)-1);break;}
    assert(s&&n<=sizeof(s->bytes));memcpy(s->bytes,data,n);s->size=n;return 0;
  }
};
struct SeedState {unsigned calls=0;bool success=true;};
bool seed(SeedState &state,OnlineNodeWindow &window,uint32_t self){
  ++state.calls;if(!state.success)return false;
  window.clear();window.setSelf(self);
  return window.create(self)&&window.create(Peer);
}
Storage storage(){Storage s;s.value.node=Self;for(unsigned i=0;i<32;++i)s.value.publicKey[i]=uint8_t(i+1);return s;}
void preparedBindingPersistsAndLoads(){
  auto st=storage();Backend b;HotNodeMetadataJournal j;OnlineNodeWindow w;SeedState seedState;Binding prepared{};uint8_t key[32]{};
  auto init=startHotNodeMetadata(st,true,false,Self,prepared,key,j,b,w,
    [&](OnlineNodeWindow &out,uint32_t self){return seed(seedState,out,self);});
  assert(init.state==HotNodeMetadataStartupState::JournalInitialized&&init.countAvailable()&&j.state()==HotNodeMetadataJournal::State::Loaded);
  assert(st.calls==1&&b.reads==3&&b.writes==1&&seedState.calls==1&&j.generation()==1&&w.hotCount()==2);
  HotNodeMetadataJournal reload;OnlineNodeWindow loaded;Binding prepared2{};uint8_t key2[32]{};SeedState unused;
  auto again=startHotNodeMetadata(st,true,false,Self,prepared2,key2,reload,b,loaded,
    [&](OnlineNodeWindow &out,uint32_t self){return seed(unused,out,self);});
  assert(again.state==HotNodeMetadataStartupState::JournalLoaded&&reload.state()==HotNodeMetadataJournal::State::Loaded&&b.writes==1);
  assert(unused.calls==0&&loaded.hotCount()==2&&loaded.self()==Self&&prepared2.node==Self&&!memcmp(key2,st.value.publicKey,32));
  puts("PASS prepared public snapshot anchors initial journal and validates later load");
}
void publicOnlyIsRamOnly(){
  auto st=storage();Backend b;HotNodeMetadataJournal j;OnlineNodeWindow w;Binding prepared{};uint8_t key[32];memset(key,0xa5,sizeof(key));SeedState ss;
  auto result=startHotNodeMetadata(st,false,true,Self,prepared,key,j,b,w,
    [&](OnlineNodeWindow &out,uint32_t self){return seed(ss,out,self);});
  assert(result.state==HotNodeMetadataStartupState::PublicRamOnly&&result.countAvailable()&&j.state()==HotNodeMetadataJournal::State::Unloaded);
  assert(!st.calls&&!b.reads&&!b.writes&&j.state()==HotNodeMetadataJournal::State::Unloaded&&w.hotCount()==2&&ss.calls==1);
  for(uint8_t x:key)assert(!x);for(uint8_t x:prepared.publicKey)assert(!x);assert(!prepared.node);
  puts("PASS empty/public-only startup seeds RAM without journal reads or writes");
}
void mismatchedOrUnavailablePreparedBindingFailsClosed(){
  auto st=storage();Backend b;HotNodeMetadataJournal j;OnlineNodeWindow w;assert(w.create(Peer));w.setSelf(Peer);auto old=w;Binding prepared{};uint8_t key[32];memset(key,0xa5,sizeof(key));SeedState ss;
  st.value.node=Self+1;
  auto bad=startHotNodeMetadata(st,true,false,Self,prepared,key,j,b,w,[&](OnlineNodeWindow &o,uint32_t n){return seed(ss,o,n);});
  assert(!bad.countAvailable()&&b.writes==0&&st.calls==1&&!b.reads&&!b.writes&&!ss.calls&&w.hotCount()==old.hotCount()&&w.self()==old.self());
  st=storage();st.available=false;HotNodeMetadataJournal noGetter;
  auto missing=startHotNodeMetadata(st,true,false,Self,prepared,key,noGetter,b,w,[&](OnlineNodeWindow &o,uint32_t n){return seed(ss,o,n);});
  assert(!missing.countAvailable()&&st.calls==1&&!b.reads&&!b.writes&&!ss.calls);
  puts("PASS bad self binding or prepared-binding getter failure leaves count unavailable and storage untouched");
}
void journalIoAndCorruptionDoNotSeedOrWrite(){
  auto st=storage();Backend b;b.readError=-EIO;HotNodeMetadataJournal io;OnlineNodeWindow w;Binding prepared{};uint8_t key[32]{};SeedState ss;
  auto failed=startHotNodeMetadata(st,true,false,Self,prepared,key,io,b,w,[&](OnlineNodeWindow &o,uint32_t n){return seed(ss,o,n);});
  assert(!failed.countAvailable()&&io.state()==HotNodeMetadataJournal::State::IoError&&b.reads==1&&!b.writes&&!ss.calls&&w.hotCount()==0);
  b.readError=0;HotNodeMetadataJournal initial;OnlineNodeWindow source;
  auto first=startHotNodeMetadata(st,true,false,Self,prepared,key,initial,b,source,[&](OnlineNodeWindow &o,uint32_t n){return seed(ss,o,n);});
  assert(first.state==HotNodeMetadataStartupState::JournalInitialized&&b.writes==1);
  auto *record=b.find("mt_hot/meta0");assert(record);record->bytes[20]^=1;const unsigned seedCalls=ss.calls;
  HotNodeMetadataJournal corrupt;OnlineNodeWindow target;assert(target.create(Peer));target.setSelf(Peer);
  auto rejected=startHotNodeMetadata(st,true,false,Self,prepared,key,corrupt,b,target,[&](OnlineNodeWindow &o,uint32_t n){return seed(ss,o,n);});
  assert(!rejected.countAvailable()&&corrupt.state()==HotNodeMetadataJournal::State::Invalid&&ss.calls==seedCalls&&b.writes==1&&target.hotCount()==1&&target.self()==Peer);
  puts("PASS storage read error and corrupt journal freeze startup without RAM publication or repair");
}
void recoveredJournalRemainsCountableButReadOnly(){
  auto st=storage();Backend b;HotNodeMetadataJournal first;OnlineNodeWindow initial;Binding prepared{};uint8_t key[32]{};SeedState seedState;
  auto made=startHotNodeMetadata(st,true,false,Self,prepared,key,first,b,initial,[&](OnlineNodeWindow &o,uint32_t n){return seed(seedState,o,n);});
  assert(made.state==HotNodeMetadataStartupState::JournalInitialized);
  auto changed=initial;assert(changed.setOrigin(Peer,false));assert(first.save(b,changed,Self,key)==0&&b.writes==2);
  auto *newest=b.find("mt_hot/meta1");assert(newest);newest->bytes[20]^=1;const unsigned writes=b.writes;
  HotNodeMetadataJournal recovered;OnlineNodeWindow loaded;Binding prepared2{};uint8_t key2[32]{};SeedState unused;
  auto result=startHotNodeMetadata(st,true,false,Self,prepared2,key2,recovered,b,loaded,[&](OnlineNodeWindow &o,uint32_t n){return seed(unused,o,n);});
  assert(result.state==HotNodeMetadataStartupState::JournalRecovered&&result.countAvailable()&&recovered.state()==HotNodeMetadataJournal::State::Recovered);
  assert(unused.calls==0&&b.writes==writes&&loaded.hotCount()==2&&loaded.self()==Self);
  puts("PASS recovered valid snapshot remains countable without automatic repair writes");
}
void uncertainGenesisWriteNeverEnablesPersistence(){
  auto st=storage();Backend b;b.writeError=-EIO;HotNodeMetadataJournal j;OnlineNodeWindow w;Binding prepared{};uint8_t key[32]{};SeedState ss;
  auto result=startHotNodeMetadata(st,true,false,Self,prepared,key,j,b,w,[&](OnlineNodeWindow &o,uint32_t n){return seed(ss,o,n);});
  assert(result.state==HotNodeMetadataStartupState::JournalInitialWriteUncertain&&result.countAvailable()&&j.state()==HotNodeMetadataJournal::State::IoError);
  assert(b.writes==1&&ss.calls==1&&w.hotCount()==2);
  puts("PASS uncertain initial commit leaves coherent RAM count but disables retry/persistence");
}
}
int main(){preparedBindingPersistsAndLoads();publicOnlyIsRamOnly();mismatchedOrUnavailablePreparedBindingFailsClosed();journalIoAndCorruptionDoNotSeedOrWrite();recoveredJournalRemainsCountableButReadOnly();uncertainGenesisWriteNeverEnablesPersistence();return 0;}
