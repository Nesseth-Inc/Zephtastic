// SPDX-License-Identifier: GPL-3.0-only
#include "notifications/NotificationJournal.h"
#include "ModuleJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace mt;
using Bytes=std::vector<uint8_t>;
struct Backend:SettingsBackend {
  enum class Fault{None,ReadError,BeforeWrite,CommittedError,TornError,TornSuccess,ReadbackError,ReadbackShort,ReadbackDifferent};
  std::map<std::string,Bytes> rows;
  std::vector<std::string> reads,writes;
  Fault fault=Fault::None;bool afterWrite=false;
  int read(const char *key,uint8_t *out,size_t cap)override {
    reads.emplace_back(key);
    if(fault==Fault::ReadError||(afterWrite&&fault==Fault::ReadbackError))return -EIO;
    const auto it=rows.find(key);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    const auto count=it->second.size();if(count)memcpy(out,it->second.data(),count);
    if(afterWrite&&fault==Fault::ReadbackDifferent&&count)out[count-1]^=1;
    return int(afterWrite&&fault==Fault::ReadbackShort&&count?count-1:count);
  }
  int write(const char *key,const uint8_t *data,size_t count)override {
    writes.emplace_back(key);afterWrite=true;
    if(fault==Fault::BeforeWrite)return -EIO;
    rows[key]={data,data+count};
    if(fault==Fault::TornError||fault==Fault::TornSuccess)rows[key].resize(11);
    return fault==Fault::CommittedError||fault==Fault::TornError?-EIO:0;
  }
};
static unsigned groups;
static void pass(const char *label){++groups;std::printf("PASS %s\n",label);}
static NotificationSettings settings(unsigned pin){NotificationSettings n;assert(n.set(true,true,false,true,pin,pin*17,pin));return n;}
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c&1)?(c>>1)^0xedb88320u:c>>1;}return ~c;}
static void seal(Bytes &b){assert(b.size()==32);Config::put32(b.data()+28,crc32(b.data(),28));}
static Backend two(){Backend b;NotificationJournal j;assert(!j.load(b)&&!j.save(b,settings(1))&&!j.save(b,settings(2)));b.afterWrite=false;return b;}
static void failClosed(const Bytes &record){Backend b;b.rows["mtnotify/state0"]=record;const auto before=b.rows;NotificationJournal j;assert(j.load(b)==-EINVAL&&!j.ready()&&j.state()==NotificationJournal::State::Invalid&&!j.config().enabled()&&b.writes.empty()&&b.rows==before);}
int main(){
  {
    Backend b;NotificationJournal j;assert(!j.ready()&&j.save(b,settings(1))==-EACCES&&b.reads.empty()&&b.writes.empty());
    assert(!j.load(b)&&j.ready()&&j.state()==NotificationJournal::State::Fresh&&j.generation()==0&&j.activeSlot()==-1);
    assert(b.writes.empty()&&b.rows.empty()&&!j.config().enabled()&&!j.config().outputMs());
    assert(b.reads==std::vector<std::string>({"mtnotify/state0","mtnotify/state1"}));
    pass("missing notification keys load disabled defaults with no write; unloaded saves are refused");
  }
  {
    auto b=two();const auto &record=b.rows.at("mtnotify/state1");const auto candidate=settings(2);
    assert(record.size()==32&&record[0]=='Z'&&record[1]=='T'&&record[2]=='N'&&record[3]==1);
    assert(Config::get32(record.data()+4)==2&&record[8]==16&&!record[9]&&!record[10]&&!record[11]);
    assert(!memcmp(record.data()+12,candidate.bytes,16)&&Config::get32(record.data()+28)==crc32(record.data(),28));
    assert(b.writes==std::vector<std::string>({"mtnotify/state0","mtnotify/state1"}));
    NotificationJournal reboot;assert(!reboot.load(b)&&reboot.config().same(candidate));const auto count=b.writes.size();
    assert(!reboot.save(b,candidate)&&b.writes.size()==count&&reboot.generation()==2&&reboot.activeSlot()==1);
    assert(!reboot.save(b,settings(3))&&reboot.generation()==3&&reboot.activeSlot()==0);
    pass("exact32-byte numeric-version envelope, alternating verified saves, restart and durable no-op");
  }
  {
    Backend b;ModuleJournal old;assert(!old.load(b)&&!old.save(b,ModuleSettings{}));
    b.rows["mtbase/config"]={1,2,3,4};b.rows["mtbase/state0"]={5,6,7};const auto protectedRows=b.rows;
    b.reads.clear();b.writes.clear();NotificationJournal journal;assert(!journal.load(b)&&!journal.save(b,settings(7))&&!journal.save(b,settings(8)));
    NotificationJournal reboot;assert(!reboot.load(b)&&reboot.config().same(settings(8)));
    for(const auto &key:b.reads)assert(key=="mtnotify/state0"||key=="mtnotify/state1");
    for(const auto &key:b.writes)assert(key=="mtnotify/state0"||key=="mtnotify/state1");
    for(const auto &row:protectedRows)assert(b.rows.at(row.first)==row.second);
    ModuleJournal oldReboot;assert(!oldReboot.load(b)&&oldReboot.config().same(old.config()));
    pass("actual old ModuleJournal remains usable and every notification read/write uses only its two string keys");
  }
  {
    auto b=two();NotificationJournal j;assert(!j.load(b));const auto before=j.config();const auto rows=b.rows;const auto gen=j.generation();const auto slot=j.activeSlot();const auto writes=b.writes.size();
    auto bad=before;bad.bytes[4]|=0x80;bad.seal();assert(j.save(b,bad)==-EINVAL&&j.ready()&&j.config().same(before)&&j.generation()==gen&&j.activeSlot()==slot&&b.rows==rows&&b.writes.size()==writes);
    pass("invalid candidate never changes durable bytes, selected generation/slot or old RAM");
  }
  {
    const auto good=two().rows.at("mtnotify/state0");
    for(unsigned offset:{0u,1u,2u,3u,8u,9u,10u,11u}){auto bad=good;bad[offset]^=1;seal(bad);failClosed(bad);}
    for(unsigned field:{4u,5u,6u,10u}){auto bad=good;NotificationSettings inner;memcpy(inner.bytes,bad.data()+12,16);
      if(field==4)inner.bytes[4]|=0x20;
      if(field==5)inner.bytes[5]=64;
      if(field==6)Config::put32(inner.bytes+6,0x80000000u);
      if(field==10)Config::put32(inner.bytes+10,NotificationSettings::MaxNagTimeoutSeconds+1);
      inner.seal();memcpy(bad.data()+12,inner.bytes,16);seal(bad);failClosed(bad);
    }
    auto bad=good;bad[14]^=1;seal(bad);failClosed(bad);
    pass("correct outer CRC does not bypass envelope reserved/version/length or inner flags/timer/pin/CRC validation");
  }
  {
    const auto full=two();const auto old=settings(1);
    for(size_t cut=0;cut<32;++cut){auto b=full;b.rows["mtnotify/state1"].resize(cut);const auto bytes=b.rows;NotificationJournal j;
      assert(!j.load(b)&&j.state()==NotificationJournal::State::Recovered&&j.config().same(old)&&j.generation()==1&&j.activeSlot()==0&&b.rows==bytes&&b.writes.size()==2);
      auto only=b.rows.at("mtnotify/state0");only.resize(cut);failClosed(only);
    }
    auto oversized=full.rows.at("mtnotify/state0");oversized.push_back(0);failClosed(oversized);
    pass("all32 truncation boundaries recover the old valid slot or fail closed when alone; oversized records never normalize");
  }
  {
    const auto full=two();
    for(unsigned offset=0;offset<32;++offset){auto b=full;b.rows["mtnotify/state1"][offset]^=1;const auto bytes=b.rows;NotificationJournal j;
      assert(!j.load(b)&&j.state()==NotificationJournal::State::Recovered&&j.config().same(settings(1))&&b.rows==bytes);
    }
    pass("single-byte corruption at every outer/inner/CRC byte falls back read-only to verified old state");
  }
  {
    auto b=two();Config::put32(b.rows["mtnotify/state1"].data()+4,1);seal(b.rows["mtnotify/state1"]);NotificationJournal j;
    assert(j.load(b)==-EINVAL&&!j.ready()&&j.save(b,settings(3))==-EACCES);
    b.rows["mtnotify/state1"]=b.rows["mtnotify/state0"];assert(!j.load(b)&&j.activeSlot()==0&&j.generation()==1);
    Config::put32(b.rows["mtnotify/state1"].data()+4,0x80000001u);seal(b.rows["mtnotify/state1"]);assert(j.load(b)==-EINVAL&&!j.ready());
    std::swap(b.rows["mtnotify/state0"],b.rows["mtnotify/state1"]);assert(j.load(b)==-EINVAL);
    pass("same-generation conflict and either half-range order refuse; identical generation/content selects deterministically");
  }
  {
    Backend b;NotificationJournal j;assert(!j.load(b)&&!j.save(b,settings(1)));
    Config::put32(b.rows["mtnotify/state0"].data()+4,UINT32_MAX);seal(b.rows["mtnotify/state0"]);
    assert(!j.load(b)&&j.generation()==UINT32_MAX&&!j.save(b,settings(2))&&j.generation()==0&&j.activeSlot()==1);
    NotificationJournal reboot;assert(!reboot.load(b)&&reboot.generation()==0&&reboot.config().same(settings(2)));
    std::swap(b.rows["mtnotify/state0"],b.rows["mtnotify/state1"]);assert(!reboot.load(b)&&reboot.activeSlot()==0&&reboot.generation()==0);
    pass("generation wraps UINT32_MAX to0 and reload order is correct in either physical slot");
  }
  {
    for(auto fault:{Backend::Fault::BeforeWrite,Backend::Fault::CommittedError,Backend::Fault::TornError,Backend::Fault::TornSuccess,Backend::Fault::ReadbackError,Backend::Fault::ReadbackShort,Backend::Fault::ReadbackDifferent}){
      Backend b;NotificationJournal j;assert(!j.load(b)&&!j.save(b,settings(1)));const auto old=j.config();const auto generation=j.generation();const auto slot=j.activeSlot();b.fault=fault;
      assert(j.save(b,settings(2))<0&&j.state()==NotificationJournal::State::IoError&&!j.ready()&&j.config().same(old)&&j.generation()==generation&&j.activeSlot()==slot);
      const auto attempts=b.writes.size();const auto bytes=b.rows;
      assert(j.save(b,old)==-EACCES&&j.save(b,settings(2))==-EACCES&&b.writes.size()==attempts&&b.rows==bytes);
      b.fault=Backend::Fault::None;b.afterWrite=false;NotificationJournal reboot;assert(!reboot.load(b));
      const bool persisted=fault==Backend::Fault::CommittedError||fault==Backend::Fault::ReadbackError||fault==Backend::Fault::ReadbackShort||fault==Backend::Fault::ReadbackDifferent;
      assert(reboot.config().same(settings(persisted?2:1)));
    }
    pass("all uncertain before/after/torn/readback saves preserve old RAM and poison even stale no-ops until reload, with no retry");
  }
  {
    Backend b;b.fault=Backend::Fault::ReadError;NotificationJournal j;assert(j.load(b)==-EIO&&!j.ready()&&j.state()==NotificationJournal::State::IoError&&!j.config().enabled()&&b.writes.empty());
    assert(j.save(b,settings(1))==-EACCES&&b.writes.empty());b.fault=Backend::Fault::None;assert(!j.load(b)&&j.ready()&&!j.save(b,settings(1)));
    b.fault=Backend::Fault::ReadError;assert(j.load(b)==-EIO&&!j.ready()&&!j.config().enabled());
    pass("backend load I/O errors remain closed with no write; explicit successful reload is required");
  }
  std::printf("NOTIFICATION_JOURNAL_GROUPS %u\n",groups);
}
