// SPDX-License-Identifier: GPL-3.0-only
// Actual production holder/cache/journal. No protocol substitute, clock or radio.
#include "NodeJournal.h"
#include "PeerDeviceMetrics.h"
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

static unsigned checks=0,groups=0,newCalls=0;
void *operator new(size_t size) {++newCalls;if(void *p=malloc(size?size:1))return p;throw std::bad_alloc();}
void *operator new[](size_t size) {return ::operator new(size);}
void operator delete(void *p) noexcept {free(p);}
void operator delete[](void *p) noexcept {free(p);}
void operator delete(void *p,size_t) noexcept {free(p);}
void operator delete[](void *p,size_t) noexcept {free(p);}
#define CHECK(value) do {++checks;if(!(value)){fprintf(stderr,"FAIL line=%d check=%s\n",__LINE__,#value);abort();}}while(0)

using Cache=mt::NodeCache;
using Entry=Cache::Entry;
using Journal=mt::NodeJournal;
using Holder=mt::PeerDeviceMetrics;
static constexpr uint32_t Self=0x10203040;

// The exact five nanopb-style flag/value pairs consumed by the templated API.
// This fixture checks the production holder, not protobuf encode/decode.
struct Metrics {
  bool has_battery_level=false;uint32_t battery_level=0;
  bool has_voltage=false;float voltage=0;
  bool has_channel_utilization=false;float channel_utilization=0;
  bool has_air_util_tx=false;float air_util_tx=0;
  bool has_uptime_seconds=false;uint32_t uptime_seconds=0;
};
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
static Metrics full(uint32_t tag=1){
  Metrics m;m.has_battery_level=m.has_voltage=m.has_channel_utilization=m.has_air_util_tx=m.has_uptime_seconds=true;
  m.battery_level=tag;m.voltage=3.125f;m.channel_utilization=12.5f;m.air_util_tx=0.25f;m.uptime_seconds=UINT32_MAX-tag;return m;
}
static Holder holder(const Metrics &m){Holder h;CHECK(h.assign(m));CHECK(h.present());return h;}
static void snapshot(const Holder &h,const Metrics &expected){
  Metrics out{};h.exportTo(out);
  const bool present=expected.has_battery_level||expected.has_voltage||expected.has_channel_utilization||expected.has_air_util_tx||expected.has_uptime_seconds;
  CHECK(h.present()==present);
  CHECK(out.has_battery_level==expected.has_battery_level);CHECK(out.battery_level==(expected.has_battery_level?expected.battery_level:0));
  CHECK(out.has_voltage==expected.has_voltage);CHECK(out.voltage==(expected.has_voltage?expected.voltage:0));
  CHECK(out.has_channel_utilization==expected.has_channel_utilization);CHECK(out.channel_utilization==(expected.has_channel_utilization?expected.channel_utilization:0));
  CHECK(out.has_air_util_tx==expected.has_air_util_tx);CHECK(out.air_util_tx==(expected.has_air_util_tx?expected.air_util_tx:0));
  CHECK(out.has_uptime_seconds==expected.has_uptime_seconds);CHECK(out.uptime_seconds==(expected.has_uptime_seconds?expected.uptime_seconds:0));
  if(expected.has_voltage&&expected.voltage==0)CHECK(std::signbit(out.voltage)==std::signbit(expected.voltage));
  if(expected.has_channel_utilization&&expected.channel_utilization==0)CHECK(std::signbit(out.channel_utilization)==std::signbit(expected.channel_utilization));
  if(expected.has_air_util_tx&&expected.air_util_tx==0)CHECK(std::signbit(out.air_util_tx)==std::signbit(expected.air_util_tx));
}
static void absent(const Holder &h){snapshot(h,Metrics{});Holder empty{};CHECK(!memcmp(&h,&empty,sizeof(h)));}
static Entry peer(uint32_t id,const char *name="Peer"){
  Entry e;e.identity(id,81,1);
  CHECK(Entry::text(e.bytes+Entry::Name,40,name,strlen(name)+1));
  CHECK(Entry::text(e.bytes+Entry::Short,5,"P",2));CHECK(Entry::valid(e.bytes,Self));return e;
}
static const Entry &at(const Cache &cache,uint32_t id){const int slot=cache.find(id);CHECK(slot>=0);return *cache.get(unsigned(slot));}
static void seed(Cache &cache,Backend &backend,Journal &journal,unsigned count=1){
  CHECK(journal.load(backend,cache,Self)==0);
  for(unsigned i=0;i<count;++i)CHECK(cache.accept(peer(2+i),Self,-3,1)==int(i));
  CHECK(journal.flush(backend,cache,100,true,true)==1);CHECK(!cache.dirty());
}
static void observe(Cache &cache,uint32_t id,const Metrics &m){const Holder h=holder(m);CHECK(cache.observeDeviceMetrics(id,h));snapshot(at(cache,id).deviceMetrics,m);}

static void measured_sizes_and_layout(){
  printf("SIZES holder=%zu entry=%zu cache=%zu journal=%zu wire_entry=%u wire_record=%zu metrics_offset=%zu\n",sizeof(Holder),sizeof(Entry),sizeof(Cache),sizeof(Journal),unsigned(Entry::Size),Journal::RecordSize,offsetof(Entry,deviceMetrics));
  fflush(stdout);
  CHECK(std::is_trivially_copyable<Holder>::value);CHECK(sizeof(Holder)==21);CHECK(alignof(Holder)==1);
  CHECK(sizeof(Entry)==288);CHECK(alignof(Entry)==1);CHECK(sizeof(Cache)==4676);CHECK(sizeof(Journal)==4700);
  CHECK(offsetof(Entry,lastHeardBytes)==95);CHECK(offsetof(Entry,deviceMetrics)==99);CHECK(Entry::Size==92);CHECK(Journal::RecordSize==1492);
}
static void optional_flags_known_zero_and_scalar_extremes(){
  Holder empty{};absent(empty);
  for(unsigned field=0;field<5;++field){
    Metrics m;
    if(field==0)m.has_battery_level=true;
    if(field==1)m.has_voltage=true;
    if(field==2)m.has_channel_utilization=true;
    if(field==3)m.has_air_util_tx=true;
    if(field==4)m.has_uptime_seconds=true;
    snapshot(holder(m),m); // Each explicitly known zero must remain present.
  }
  Metrics m=full(UINT32_MAX);m.uptime_seconds=UINT32_MAX;m.voltage=FLT_MAX;m.channel_utilization=100;m.air_util_tx=100;snapshot(holder(m),m);
  m=full();m.voltage=-0.0f;m.channel_utilization=-0.0f;m.air_util_tx=-0.0f;snapshot(holder(m),m);
  m=full();m.voltage=0.125f;m.channel_utilization=99.875f;m.air_util_tx=0.03125f;snapshot(holder(m),m);
}
static void invalid_atomic_and_absent_numeric_ignored(){
  Holder h=holder(full(77));const Holder before=h;Metrics none;
  CHECK(!h.assign(none));CHECK(!memcmp(&h,&before,sizeof(h)));
  const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
  for(unsigned field=0;field<3;++field)for(float bad:{nan,inf,-inf,-0.125f}){
    Metrics m=full();if(field==0)m.voltage=bad;if(field==1)m.channel_utilization=bad;if(field==2)m.air_util_tx=bad;
    CHECK(!h.assign(m));CHECK(!memcmp(&h,&before,sizeof(h)));
  }
  for(unsigned field=0;field<2;++field){Metrics m=full();if(field==0)m.channel_utilization=100.125f;else m.air_util_tx=100.125f;CHECK(!h.assign(m));CHECK(!memcmp(&h,&before,sizeof(h)));}
  Metrics sparse;sparse.has_battery_level=true;sparse.battery_level=UINT32_MAX;sparse.voltage=nan;sparse.channel_utilization=inf;sparse.air_util_tx=-inf;sparse.uptime_seconds=UINT32_MAX;
  CHECK(h.assign(sparse));snapshot(h,sparse);
  Metrics onlyUptime;onlyUptime.has_uptime_seconds=true;onlyUptime.uptime_seconds=0;onlyUptime.battery_level=UINT32_MAX;onlyUptime.voltage=-1;onlyUptime.channel_utilization=nan;onlyUptime.air_util_tx=inf;
  CHECK(h.assign(onlyUptime));snapshot(h,onlyUptime);
  const Holder clean=holder(onlyUptime);CHECK(!memcmp(&h,&clean,sizeof(h)));
}
static void observation_no_heap_dirty_or_backend_io(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);CHECK(cache.observeLastHeard(2,1234));
  const Entry before=at(cache,2);const unsigned reads=backend.reads,writes=backend.writes,allocated=newCalls;
  for(uint32_t tag:{0u,101u,UINT32_MAX}){
    observe(cache,2,full(tag));const Entry &e=at(cache,2);
    CHECK(!memcmp(e.bytes,before.bytes,Entry::Size));CHECK(e.lastHeard()==1234&&e.snr==before.snr&&e.hops==before.hops&&e.heard==before.heard);
    CHECK(cache.count()==1&&!cache.dirty());
  }
  journal.observe(cache,1000000);CHECK(!journal.due(2000000));CHECK(journal.flush(backend,cache,2000000,true,true)==0);
  CHECK(backend.reads==reads&&backend.writes==writes&&newCalls==allocated);
  CHECK(cache.edit(2,Self,Cache::Edit::Favorite)==1);CHECK(cache.dirty());observe(cache,2,full(9));CHECK(cache.dirty());CHECK(backend.reads==reads&&backend.writes==writes&&newCalls==allocated);
}
static void unknown_empty_and_frozen_refuse_without_mutation(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);observe(cache,2,full());const Cache before=cache;const Holder good=holder(full(8));Holder empty{};
  for(uint32_t id:{0u,1u,Self,3u,0xfffffffeu,UINT32_MAX}){CHECK(!cache.observeDeviceMetrics(id,good));CHECK(!memcmp(&before,&cache,sizeof(cache)));}
  CHECK(!cache.observeDeviceMetrics(2,empty));CHECK(!memcmp(&before,&cache,sizeof(cache)));CHECK(cache.count()==1&&!cache.dirty());
  Cache frozen;Journal failed;Backend bad;CHECK(failed.load(bad,frozen,1)==-EINVAL);CHECK(!frozen.ready());const Cache original=frozen;
  CHECK(!frozen.observeDeviceMetrics(2,good));CHECK(!memcmp(&original,&frozen,sizeof(frozen)));
}
static void ignored_and_placeholder_admission(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);observe(cache,2,full(2));
  CHECK(journal.edit(backend,cache,2,Cache::Edit::Ignore,200,true)==0);const Cache ignored=cache;
  CHECK(!cache.observeDeviceMetrics(2,holder(full(3))));CHECK(!memcmp(&ignored,&cache,sizeof(cache)));
  CHECK(journal.edit(backend,cache,2,Cache::Edit::Unignore,300,true)==0);absent(at(cache,2).deviceMetrics);
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Ignore,400,true)==0);CHECK(at(cache,3).placeholder());absent(at(cache,3).deviceMetrics);
  CHECK(!cache.observeDeviceMetrics(3,holder(full(3))));CHECK(journal.edit(backend,cache,3,Cache::Edit::Unignore,500,true)==0);
  const Cache placeholder=cache;CHECK(!cache.observeDeviceMetrics(3,holder(full(3))));CHECK(!memcmp(&placeholder,&cache,sizeof(cache)));
  CHECK(cache.accept(peer(3),Self,1,1)>=0);CHECK(!at(cache,3).placeholder());absent(at(cache,3).deviceMetrics);observe(cache,3,full(3));
}
static void same_identity_refresh_and_rejected_identity(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);observe(cache,2,full(2));CHECK(cache.observeLastHeard(2,222));const Holder old=at(cache,2).deviceMetrics;
  CHECK(cache.accept(peer(2),Self,1,2)==0);CHECK(!cache.dirty());CHECK(!memcmp(&old,&at(cache,2).deviceMetrics,sizeof(old)));
  Entry changed=peer(2,"Changed");changed.deviceMetrics=holder(full(99));CHECK(cache.accept(changed,Self,2,3)==0);CHECK(cache.dirty());CHECK(at(cache,2).lastHeard()==222);CHECK(!memcmp(&old,&at(cache,2).deviceMetrics,sizeof(old)));
  Entry invalid=peer(2);invalid.bytes[Entry::Flags]=255;const Cache before=cache;
  CHECK(cache.accept(invalid,Self,1,1)==-EINVAL);CHECK(!memcmp(&before,&cache,sizeof(cache)));
}
static void eviction_reuse_clears_whole_snapshot(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal,Cache::Capacity);
  for(uint32_t id=2;id<2+Cache::Capacity;++id)observe(cache,id,full(id));
  CHECK(cache.edit(2,Self,Cache::Edit::Favorite)==1);Entry incoming=peer(99);incoming.deviceMetrics=holder(full(99));
  CHECK(cache.accept(incoming,Self,0,0)==1);CHECK(cache.find(3)==-1);snapshot(at(cache,2).deviceMetrics,full(2));absent(at(cache,99).deviceMetrics);
  CHECK(!cache.observeDeviceMetrics(3,holder(full(3))));CHECK(cache.count()==Cache::Capacity);observe(cache,99,full(99));
  // An Ignore placeholder also reuses a populated slot and must clear it.
  CHECK(cache.edit(100,Self,Cache::Edit::Ignore)==1);CHECK(at(cache,100).placeholder());absent(at(cache,100).deviceMetrics);
  CHECK(cache.edit(100,Self,Cache::Edit::Unignore)==1);absent(at(cache,100).deviceMetrics);CHECK(cache.accept(peer(100),Self,0,0)>=0);absent(at(cache,100).deviceMetrics);
}
static void removal_shifts_metadata_and_clears_tail(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal,3);
  for(uint32_t id:{2u,3u,4u}){observe(cache,id,full(id));CHECK(cache.observeLastHeard(id,10*id));}
  CHECK(cache.edit(3,Self,Cache::Edit::Remove)==1);CHECK(cache.count()==2&&cache.find(3)==-1);
  CHECK(cache.get(1)->id()==4&&cache.get(1)->lastHeard()==40);snapshot(cache.get(1)->deviceMetrics,full(4));snapshot(at(cache,2).deviceMetrics,full(2));
  Entry incoming=peer(5);incoming.deviceMetrics=holder(full(5));CHECK(cache.accept(incoming,Self,1,1)==2);absent(at(cache,5).deviceMetrics);CHECK(at(cache,5).lastHeard()==0);
}
static void journal_wire_identical_and_reload_loses_metrics(){
  Cache a,b;Backend one,two;Journal j1,j2;CHECK(j1.load(one,a,Self)==0&&j2.load(two,b,Self)==0);
  for(uint32_t id:{2u,3u}){CHECK(a.accept(peer(id),Self,0,0)>=0);CHECK(b.accept(peer(id),Self,0,0)>=0);observe(b,id,full(id));CHECK(b.observeLastHeard(id,UINT32_MAX-id));}
  CHECK(j1.flush(one,a,100,true,true)==1&&j2.flush(two,b,100,true,true)==1);CHECK(one.data==two.data);CHECK(one.present[0]&&one.data[0][3]==3&&one.data[0][10]==92);
  for(unsigned i=0;i<b.count();++i)CHECK(!memcmp(two.data[0].data()+Journal::HeaderSize+i*Entry::Size,b.get(i)->bytes,Entry::Size));
  const auto firstGeneration=two.data[0];
  CHECK(j1.edit(one,a,2,Cache::Edit::Favorite,200,true)==0&&j2.edit(two,b,2,Cache::Edit::Favorite,200,true)==0);CHECK(one.data==two.data);
  CHECK(j1.activeSlot()==1&&j2.activeSlot()==1);CHECK(two.data[0]==firstGeneration);
  for(unsigned i=0;i<b.count();++i)CHECK(!memcmp(two.data[1].data()+Journal::HeaderSize+i*Entry::Size,b.get(i)->bytes,Entry::Size));
  // Load into the populated cache too: previous volatile values must vanish.
  Journal reboot;CHECK(reboot.load(two,b,Self)==0);CHECK(b.count()==2&&!b.dirty());
  for(uint32_t id:{2u,3u}){absent(at(b,id).deviceMetrics);CHECK(at(b,id).lastHeard()==0);CHECK(!memcmp(at(a,id).bytes,at(b,id).bytes,Entry::Size));}
}
static void journal_candidate_copy_success_and_uncertain_failure(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal,3);
  for(uint32_t id:{2u,3u,4u}){observe(cache,id,full(id));CHECK(cache.observeLastHeard(id,id*10));}
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Favorite,200,true)==0);CHECK(journal.edit(backend,cache,2,Cache::Edit::ToggleMute,300,true)==0);
  for(uint32_t id:{2u,3u,4u}){snapshot(at(cache,id).deviceMetrics,full(id));CHECK(at(cache,id).lastHeard()==id*10);}
  CHECK(journal.edit(backend,cache,3,Cache::Edit::Remove,400,true)==0);CHECK(cache.find(3)==-1);snapshot(at(cache,4).deviceMetrics,full(4));snapshot(at(cache,2).deviceMetrics,full(2));
  backend.errorAfterCommit=true;const Entry prior=at(cache,2);CHECK(journal.edit(backend,cache,2,Cache::Edit::ToggleMute,500,true)==-EIO);CHECK(!cache.ready());
  const Cache frozen=cache;const unsigned writes=backend.writes;CHECK(!cache.observeDeviceMetrics(2,holder(full(99))));CHECK(!memcmp(&frozen,&cache,sizeof(cache)));CHECK(backend.writes==writes);
  CHECK(!memcmp(&prior,&at(cache,2),sizeof(prior)));snapshot(at(cache,4).deviceMetrics,full(4));
}
static void repeated_fanout_and_whole_snapshot_replacement(){
  Cache cache;Backend backend;Journal journal;seed(cache,backend,journal);observe(cache,2,full(2));const Cache first=cache;
  CHECK(cache.observeDeviceMetrics(2,holder(full(2))));CHECK(!memcmp(&first,&cache,sizeof(cache)));
  Metrics onlyVoltage;onlyVoltage.has_voltage=true;onlyVoltage.voltage=-0.0f;observe(cache,2,onlyVoltage);const Holder fresh=holder(onlyVoltage);
  CHECK(!memcmp(&fresh,&at(cache,2).deviceMetrics,sizeof(fresh)));CHECK(!cache.dirty());
  Holder rejected=at(cache,2).deviceMetrics;Metrics invalid=full();invalid.air_util_tx=101;CHECK(!rejected.assign(invalid));
  CHECK(!memcmp(&fresh,&rejected,sizeof(fresh)));CHECK(cache.observeDeviceMetrics(2,rejected));snapshot(at(cache,2).deviceMetrics,onlyVoltage);
}
int main(){
  const struct {const char *name;void(*run)();} tests[]={
    {"measured_host_and_wire_layout",measured_sizes_and_layout},
    {"five_optional_flags_zero_full_uint32_float_boundaries",optional_flags_known_zero_and_scalar_extremes},
    {"invalid_atomic_absent_numeric_ignored",invalid_atomic_and_absent_numeric_ignored},
    {"observation_no_heap_dirty_or_store",observation_no_heap_dirty_or_backend_io},
    {"unknown_empty_frozen_no_mutation",unknown_empty_and_frozen_refuse_without_mutation},
    {"ignored_placeholder_admission",ignored_and_placeholder_admission},
    {"same_identity_refresh_preserves",same_identity_refresh_and_rejected_identity},
    {"eviction_placeholder_reuse_clears",eviction_reuse_clears_whole_snapshot},
    {"removal_shifts_and_tail_clears",removal_shifts_metadata_and_clears_tail},
    {"journal_wire_identical_reload_unknown",journal_wire_identical_and_reload_loses_metrics},
    {"candidate_copy_and_uncertain_failure",journal_candidate_copy_success_and_uncertain_failure},
    {"fanout_idempotence_whole_snapshot",repeated_fanout_and_whole_snapshot_replacement}};
  for(const auto &test:tests){test.run();++groups;printf("PASS %s\n",test.name);}
  printf("SUMMARY groups=%u checks=%u failures=0\n",groups,checks);return 0;
}
