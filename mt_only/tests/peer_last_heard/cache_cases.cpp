// SPDX-License-Identifier: GPL-3.0-only
// Actual production cache/journal, bounded in-memory backend; no radio or clock.
#include "NodeJournal.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

static unsigned checks=0,groups=0,newCalls=0;
void *operator new(size_t size) { ++newCalls;if(void *p=malloc(size?size:1))return p;throw std::bad_alloc(); }
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p,size_t) noexcept { free(p); }
void operator delete[](void *p,size_t) noexcept { free(p); }
#define CHECK(value) do {++checks;if(!(value)){fprintf(stderr,"FAIL line=%d check=%s\n",__LINE__,#value);abort();}}while(0)

using Cache=mt::NodeCache;
using Entry=Cache::Entry;
using Journal=mt::NodeJournal;
static constexpr uint32_t Self=0x10203040;
struct Backend final:mt::SettingsBackend {
  std::array<std::array<uint8_t,Journal::RecordSize>,2> data{};
  bool present[2]{},errorAfterCommit=false;unsigned reads=0,writes=0;
  static unsigned slot(const char *key){CHECK(!strcmp(key,"mtbase/nodes0")||!strcmp(key,"mtbase/nodes1"));return key[12]=='1';}
  int read(const char *key,uint8_t *out,size_t capacity)override{
    ++reads;const unsigned i=slot(key);if(!present[i])return -ENOENT;
    if(capacity<Journal::RecordSize)return -E2BIG;
    memcpy(out,data[i].data(),Journal::RecordSize);return int(Journal::RecordSize);
  }
  int write(const char *key,const uint8_t *in,size_t length)override{
    CHECK(length==Journal::RecordSize);++writes;const unsigned i=slot(key);
    memcpy(data[i].data(),in,length);present[i]=true;return errorAfterCommit?-EIO:0;
  }
};
static Entry peer(uint32_t id,const char *name="Peer"){
  Entry e;e.identity(id,81,1);
  CHECK(Entry::text(e.bytes+Entry::Name,40,name,strlen(name)+1));
  CHECK(Entry::text(e.bytes+Entry::Short,5,"P",2));
  CHECK(Entry::valid(e.bytes,Self));return e;
}
static const Entry &at(const Cache &cache,uint32_t id){
  const int slot=cache.find(id);CHECK(slot>=0);return *cache.get(unsigned(slot));
}
static void seed(Cache &cache,Backend &backend,Journal &journal,unsigned count=1){
  CHECK(journal.load(backend,cache,Self)==0);
  for(unsigned i=0;i<count;++i)CHECK(cache.accept(peer(2+i),Self,-3,1)==int(i));
  CHECK(journal.flush(backend,cache,100,true,true)==1);CHECK(!cache.dirty());
}
static void sizes(){
  CHECK(Entry::Size==92);CHECK(Journal::RecordSize==1492);
  CHECK(sizeof(Entry)==288);CHECK(alignof(Entry)==1);CHECK(sizeof(Cache)==4676);
  CHECK(sizeof(Journal)==4700);CHECK(offsetof(Entry,lastHeardBytes)==95);
  printf("SIZES entry=%zu cache=%zu journal=%zu wire_entry=%u wire_record=%zu\n",
         sizeof(Entry),sizeof(Cache),sizeof(Journal),unsigned(Entry::Size),Journal::RecordSize);
}
static void scalar_policy_and_unknown(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);
  const unsigned writes=backend.writes,reads=backend.reads,allocated=newCalls;
  const auto before=at(cache,2);CHECK(cache.observeLastHeard(2,0));CHECK(at(cache,2).lastHeard()==0);
  for(uint32_t value:{123u,123u,124u,17u,UINT32_MAX,1u}){
    CHECK(cache.observeLastHeard(2,value));CHECK(at(cache,2).lastHeard()==value);
    CHECK(cache.observeLastHeard(2,0));CHECK(at(cache,2).lastHeard()==value);
  }
  CHECK(!memcmp(before.bytes,at(cache,2).bytes,Entry::Size));
  CHECK(at(cache,2).snr==before.snr&&at(cache,2).hops==before.hops&&at(cache,2).heard==before.heard);
  CHECK(!cache.dirty());CHECK(newCalls==allocated);
  journal.observe(cache,1000000);CHECK(!journal.due(2000000));
  CHECK(journal.flush(backend,cache,2000000,true,true)==0);
  CHECK(backend.writes==writes&&backend.reads==reads);
}
static void invalid_unknown_refuse_without_mutation(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);
  CHECK(cache.observeLastHeard(2,77));const Cache before=cache;
  for(uint32_t id:{0u,1u,Self,3u,0xfffffffeu,UINT32_MAX}){
    CHECK(!cache.observeLastHeard(id,123));CHECK(!memcmp(&before,&cache,sizeof(cache)));
  }
  CHECK(cache.count()==1&&!cache.dirty());
}
static void ignored_and_placeholder_refuse(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);
  CHECK(cache.observeLastHeard(2,77));CHECK(journal.edit(backend,cache,2,Cache::Edit::Ignore,200,true)==0);
  const Cache ignored=cache;CHECK(!cache.observeLastHeard(2,88));CHECK(!memcmp(&ignored,&cache,sizeof(cache)));
  CHECK(journal.edit(backend,cache,2,Cache::Edit::Unignore,300,true)==0);
  CHECK(cache.observeLastHeard(2,0));CHECK(at(cache,2).lastHeard()==77);
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Ignore,400,true)==0);
  CHECK(at(cache,3).placeholder());CHECK(!cache.observeLastHeard(3,88));
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Unignore,500,true)==0);
  const Cache placeholder=cache;CHECK(!cache.observeLastHeard(3,88));CHECK(!memcmp(&placeholder,&cache,sizeof(cache)));
  CHECK(cache.accept(peer(3),Self,0,0)>=0);CHECK(!at(cache,3).placeholder());
  CHECK(cache.observeLastHeard(3,88));CHECK(at(cache,3).lastHeard()==88);
}
static void accepted_identity_refresh_preserves(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);
  CHECK(cache.observeLastHeard(2,100));CHECK(cache.accept(peer(2),Self,0,2)==0);
  CHECK(!cache.dirty());CHECK(at(cache,2).lastHeard()==100);
  CHECK(cache.accept(peer(2,"Changed"),Self,2,3)==0);CHECK(cache.dirty());
  CHECK(at(cache,2).lastHeard()==100);CHECK(cache.observeLastHeard(2,0));
  Entry invalid=peer(2);invalid.bytes[Entry::Flags]=255;const Cache before=cache;
  CHECK(cache.accept(invalid,Self,1,1)==-EINVAL);CHECK(!memcmp(&before,&cache,sizeof(cache)));
}
static void eviction_clears_new_identity_before_unknown(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal,Cache::Capacity);
  for(uint32_t id=2;id<2+Cache::Capacity;++id)CHECK(cache.observeLastHeard(id,100+id));
  CHECK(cache.edit(2,Self,Cache::Edit::Favorite)==1);
  CHECK(cache.accept(peer(99),Self,0,0)==1);CHECK(cache.find(3)==-1);
  CHECK(at(cache,2).favorite()&&at(cache,2).lastHeard()==102);
  CHECK(at(cache,99).lastHeard()==0);CHECK(cache.observeLastHeard(99,0));CHECK(at(cache,99).lastHeard()==0);
  CHECK(!cache.observeLastHeard(3,999));CHECK(cache.count()==Cache::Capacity);
}
static void remove_shifts_all_metadata_and_clears_tail(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal,3);
  CHECK(cache.observeLastHeard(2,20)&&cache.observeLastHeard(3,30)&&cache.observeLastHeard(4,40));
  CHECK(cache.edit(3,Self,Cache::Edit::Remove)==1);CHECK(cache.count()==2&&cache.find(3)==-1);
  CHECK(cache.get(1)->id()==4&&cache.get(1)->lastHeard()==40);CHECK(at(cache,2).lastHeard()==20);
  CHECK(cache.accept(peer(5),Self,1,1)==2);CHECK(at(cache,5).lastHeard()==0);
}
static void journal_wire_identical_and_restore_zero(){
  Cache a,b;Backend one,two;Journal j1,j2;
  CHECK(j1.load(one,a,Self)==0&&j2.load(two,b,Self)==0);
  for(uint32_t id:{2u,3u}){CHECK(a.accept(peer(id),Self,0,0)>=0);CHECK(b.accept(peer(id),Self,0,0)>=0);CHECK(b.observeLastHeard(id,UINT32_MAX-id));}
  CHECK(j1.flush(one,a,100,true,true)==1&&j2.flush(two,b,100,true,true)==1);
  CHECK(one.data==two.data);CHECK(one.present[0]&&one.data[0][3]==3&&one.data[0][10]==92);
  Cache restored;Journal reboot;CHECK(reboot.load(two,restored,Self)==0);
  CHECK(restored.count()==2&&!restored.dirty());
  for(uint32_t id:{2u,3u}){CHECK(at(restored,id).lastHeard()==0);CHECK(!memcmp(at(restored,id).bytes,at(b,id).bytes,Entry::Size));}
}
static void journal_candidate_copy_success_and_failure(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal,3);
  for(uint32_t id:{2u,3u,4u})CHECK(cache.observeLastHeard(id,id*10));
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Favorite,200,true)==0);
  for(uint32_t id:{2u,3u,4u})CHECK(at(cache,id).lastHeard()==id*10);
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Remove,300,true)==0);
  CHECK(cache.find(3)==-1&&at(cache,4).lastHeard()==40&&at(cache,2).lastHeard()==20);
  backend.errorAfterCommit=true;
  CHECK(journal.edit(backend,cache,2,Cache::Edit::ToggleMute,400,true)==-EIO);
  CHECK(!cache.ready());const Cache frozen=cache;const unsigned writes=backend.writes;
  CHECK(!cache.observeLastHeard(2,999));CHECK(!memcmp(&frozen,&cache,sizeof(cache)));
  CHECK(at(cache,2).lastHeard()==20&&backend.writes==writes);
}
static void duplicate_fanout_is_idempotent(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);
  CHECK(cache.observeLastHeard(2,123));const Cache usb=cache;
  CHECK(cache.observeLastHeard(2,123));CHECK(!memcmp(&usb,&cache,sizeof(cache)));
  CHECK(!cache.dirty());
}
int main(){
  const struct {const char *name;void(*run)();} tests[]={
    {"sizes_and_wire",sizes},{"scalar_zero_backward_uint32_no_store",scalar_policy_and_unknown},
    {"invalid_unknown_no_mutation",invalid_unknown_refuse_without_mutation},
    {"ignored_placeholder_admission",ignored_and_placeholder_refuse},
    {"accepted_refresh_preserves",accepted_identity_refresh_preserves},
    {"eviction_clears_before_unknown",eviction_clears_new_identity_before_unknown},
    {"remove_shifts_clears",remove_shifts_all_metadata_and_clears_tail},
    {"journal_wire_restore",journal_wire_identical_and_restore_zero},
    {"journal_copy_and_frozen_failure",journal_candidate_copy_success_and_failure},
    {"idempotent_dual_fanout",duplicate_fanout_is_idempotent}};
  for(const auto &test:tests){test.run();++groups;printf("PASS %s\n",test.name);}
  printf("SUMMARY groups=%u checks=%u failures=0\n",groups,checks);return 0;
}
