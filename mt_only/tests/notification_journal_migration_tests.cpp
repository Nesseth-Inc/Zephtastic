// SPDX-License-Identifier: GPL-3.0-only
#include "notifications/NotificationJournal.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace mt;
using Bytes=std::vector<uint8_t>;
struct Backend:SettingsBackend {
  std::map<std::string,Bytes> rows;
  unsigned writes=0;bool fail=false,committed=false;
  int read(const char*k,uint8_t*p,size_t cap)override{const auto i=rows.find(k);if(i==rows.end())return -ENOENT;if(i->second.size()>cap)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
  int write(const char*k,const uint8_t*p,size_t n)override{++writes;if(!fail||committed)rows[k]=Bytes(p,p+n);return fail?-EIO:0;}
};
static uint32_t crc32(const uint8_t*p,size_t n){uint32_t c=0xffffffff;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c&1)?(c>>1)^0xedb88320u:c>>1;}return ~c;}
static void outerSeal(Bytes&v){Config::put32(v.data()+28,crc32(v.data(),28));}
static NotificationSettings legacy(){
  // Construct the actual historical ZTN1 record, not a v2 setter projection.
  NotificationSettings s;s.bytes[3]='1';s.bytes[4]=0x0b;s.bytes[5]=28;
  Config::put32(s.bytes+6,1000);Config::put32(s.bytes+10,65535);s.seal();assert(s.valid());return s;
}
static Bytes record(const NotificationSettings&s,uint32_t generation){Bytes v(32);v[0]='Z';v[1]='T';v[2]='N';v[3]=1;Config::put32(v.data()+4,generation);v[8]=16;memcpy(v.data()+12,s.bytes,16);outerSeal(v);return v;}
static Backend original(uint32_t generation=41){Backend b;b.rows["mtnotify/state0"]=record(legacy(),generation);b.rows["mtbase/state0"]={9,8,7};return b;}
int main(){
  {
    auto b=original();const auto before=b.rows;NotificationJournal j;
    assert(!j.load(b)&&j.ready()&&j.generation()==41&&j.config().same(legacy())&&j.config().heartbeatDisabled());
    assert(b.rows==before&&!b.writes&&j.config().bytes[3]=='1');
    assert(!j.save(b,j.config())&&b.rows==before&&!b.writes);
    puts("PASS historical ZTN1 journal loads and no-ops read-only without implicit rewrite");
  }
  {
    auto b=original();const auto old=b.rows;NotificationJournal j;assert(!j.load(b));auto changed=j.config();
    assert(changed.setHeartbeatDisabled(false)&&!changed.heartbeatDisabled());
    assert(!memcmp(changed.bytes+5,legacy().bytes+5,9)&&(changed.bytes[4]&0x0f)==0x0b);
    assert(!j.save(b,changed)&&b.writes==1&&j.generation()==42&&j.activeSlot()==1);
    assert(b.rows.at("mtnotify/state0")==old.at("mtnotify/state0")&&b.rows.at("mtbase/state0")==old.at("mtbase/state0"));
    const auto&v=b.rows.at("mtnotify/state1");assert(v.size()==32&&v[3]==1&&v[8]==16&&!v[9]&&!v[10]&&!v[11]&&v[15]=='2');
    NotificationJournal boot;assert(!boot.load(b)&&boot.config().same(changed)&&!boot.config().heartbeatDisabled()&&boot.generation()==42);
    puts("PASS explicit heartbeat change persists inner ZTN2 in unchanged outer v1 envelope and reloads");
  }
  {
    auto b=original();NotificationJournal j;assert(!j.load(b));auto c=j.config();assert(c.set(true,false,true,false,0,0,0));
    assert(c.bytes[3]=='2'&&c.heartbeatDisabled()&&c.enabled()&&!c.outputMs()&&!c.nagTimeoutSeconds());
    assert(!j.save(b,c));NotificationJournal boot;assert(!boot.load(b)&&boot.config().same(c)&&boot.config().heartbeatDisabled());
    puts("PASS unrelated explicit notification edit migrates with old implied disabled heartbeat and preserves zero fields");
  }
  {
    auto full=original();auto c=legacy();assert(c.setHeartbeatDisabled(false));full.rows["mtnotify/state1"]=record(c,42);
    for(size_t cut=0;cut<32;++cut){auto b=full;b.rows["mtnotify/state1"].resize(cut);const auto before=b.rows;NotificationJournal boot;
      assert(!boot.load(b)&&boot.state()==NotificationJournal::State::Recovered&&boot.generation()==41&&boot.config().same(legacy())&&boot.config().heartbeatDisabled()&&b.rows==before&&!b.writes);}
    for(unsigned off:{9u,10u,11u,15u,16u,26u}){auto b=full;b.rows["mtnotify/state1"][off]^=0x80;outerSeal(b.rows["mtnotify/state1"]);NotificationJournal boot;
      assert(!boot.load(b)&&boot.state()==NotificationJournal::State::Recovered&&boot.config().same(legacy())&&!b.writes);}
    puts("PASS all32 torn migration boundaries and CRC-valid malformed wrappers or inner records recover literal ZTN1");
  }
  {
    for(bool committed:{false,true}){auto b=original();NotificationJournal j;assert(!j.load(b));const auto old=j.config();auto c=old;assert(c.setHeartbeatDisabled(false));b.fail=true;b.committed=committed;
      assert(j.save(b,c)==-EIO&&!j.ready()&&j.state()==NotificationJournal::State::IoError&&j.config().same(old)&&j.generation()==41);
      const auto count=b.writes;assert(j.save(b,c)==-EACCES&&b.writes==count);b.fail=false;
      NotificationJournal boot;assert(!boot.load(b)&&boot.config().heartbeatDisabled()==!committed&&boot.config().bytes[3]==(committed?'2':'1'));}
    puts("PASS failed versus committed-error migration keeps old RAM, poisons writes and requires honest reboot reconciliation");
  }
  {
    auto b=original(UINT32_MAX);NotificationJournal j;assert(!j.load(b));auto c=j.config();assert(c.setHeartbeatDisabled(false)&&!j.save(b,c)&&j.generation()==0);
    NotificationJournal boot;assert(!boot.load(b)&&boot.generation()==0&&!boot.config().heartbeatDisabled());
    b.rows["mtnotify/state0"]=record(legacy(),0);assert(boot.load(b)==-EINVAL&&!boot.ready());
    b.rows["mtnotify/state0"]=record(legacy(),0x80000000u);assert(boot.load(b)==-EINVAL&&!boot.ready());
    puts("PASS mixed-version generation wrap works while equal-generation conflict and half-range ambiguity refuse");
  }
  puts("NOTIFICATION_JOURNAL_MIGRATION_GROUPS 6");
}
