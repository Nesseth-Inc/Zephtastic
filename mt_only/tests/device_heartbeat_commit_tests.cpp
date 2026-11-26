// SPDX-License-Identifier: GPL-3.0-only
// Actual DeviceSettingsCommit/SettingsCommit/Service with an explicit RAM backend.
#include "DeviceSettingsCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using Bytes=std::vector<uint8_t>;
static constexpr uint32_t Self=0x12345678;
struct Backend:mt::SettingsBackend {
  std::map<std::string,Bytes> files;std::vector<std::string>writes;
  std::string failPrefix;bool committed=false;
  int read(const char*k,uint8_t*p,size_t cap)override{const auto i=files.find(k);if(i==files.end())return -ENOENT;if(i->second.size()>cap)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
  int write(const char*k,const uint8_t*p,size_t n)override{writes.emplace_back(k);const bool fail=!failPrefix.empty()&&std::string(k).find(failPrefix)==0;if(!fail||committed)files[k]={p,p+n};return fail?-EIO:0;}
};
struct Radio:mt::Radio {
  int state=0;unsigned sends=0;bool busy_=false;
  bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}int stop()override{state=0;return 0;}
  int health()override{return state;}bool busy()override{return busy_;}uint32_t airtime(size_t)override{return 100;}int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};
struct Fixture {
  Radio radio;Backend backend;mt::test::KnownChannelFixtureClock clock;mt::Service service;
  mt::SettingsJournal coreJournal;mt::SettingsEdit core;mt::NodeJournal nodes;mt::NotificationJournal journal;mt::NotificationEdit led;mt::SettingsCommit commit;
  Fixture():service(radio,true,Self,127,true,false),core(service,coreJournal,backend),led(service,journal,backend),commit(service,radio,core,nodes,backend,nullptr,&led){
    clock.bind(service);assert(!coreJournal.load(backend)&&!journal.load(backend)&&led.load()&&!nodes.load(backend,service.nodes,Self));
    auto c=coreJournal.config();c.bytes[4]=1;c.setTxEnabled(true);assert(c.setNodeInfoSeconds(3600));
    assert(!coreJournal.save(backend,c)&&service.apply(c)&&service.start(false,0)&&service.armEndpoint(true));backend.writes.clear();
  }
  mt::Config candidate(){auto c=service.config;assert(c.setNodeInfoSeconds(7200)&&!c.same(service.config));return c;}
  int save(const mt::Config&c,bool disabled,uint32_t now=10,bool available=true){return mt::DeviceSettingsCommit::save(service,core,led,commit,c,disabled,available,now);}
};
int main(){
  {
    Fixture f;const auto c=f.candidate();assert(!f.save(c,false)&&!f.core.open()&&!f.led.dirty()&&f.service.config.same(c)&&!f.led.config().heartbeatDisabled());
    assert(f.backend.writes.size()==2&&f.backend.writes[0].find("mtnotify/")==0&&f.backend.writes[1].find("mtbase/state")==0);
    mt::SettingsJournal core;mt::NotificationJournal led;assert(!core.load(f.backend)&&!led.load(f.backend)&&core.config().same(c)&&!led.config().heartbeatDisabled());
    assert(f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
    puts("PASS compound Device core and heartbeat persist in both actual journals with RX intent restored and no RF");
  }
  {
    Fixture f;const auto core=f.service.config;assert(!f.save(core,false));assert(f.backend.writes.size()==1&&f.backend.writes[0].find("mtnotify/")==0&&f.service.config.same(core));
    const auto count=f.backend.writes.size();assert(!f.save(core,false)&&f.backend.writes.size()==count);
    const auto led=f.led.config();assert(!f.save(f.candidate(),false)&&f.led.config().same(led)&&f.backend.writes.size()==count+1);
    puts("PASS heartbeat-only and core-only replacements touch only their journal; repeated same Device is inert");
  }
  {
    Fixture f;const auto old=f.service.config;const auto c=f.candidate();assert(!f.core.begin(10)&&!f.save(c,false,20)&&f.core.open()&&f.core.dirty()&&f.led.dirty()&&f.backend.writes.empty());
    mt::SettingsJournal oldCore;mt::NotificationJournal oldLed;assert(!oldCore.load(f.backend)&&!oldLed.load(f.backend)&&oldCore.config().same(old)&&oldLed.config().heartbeatDisabled());
    assert(f.service.config.same(c)&&!f.led.config().heartbeatDisabled());
    assert(!f.commit.commit(30)&&!f.core.open()&&!f.led.dirty()&&f.backend.writes.size()==2);
    assert(!oldCore.load(f.backend)&&!oldLed.load(f.backend)&&oldCore.config().same(c)&&!oldLed.config().heartbeatDisabled());
    puts("PASS already-open edit defers both writes while exposing RAM settings, then commits both verified records");
  }
  {
    for(bool committed:{false,true}){Fixture f;const auto old=f.service.config;const auto c=f.candidate();f.backend.failPrefix="mtnotify/";f.backend.committed=committed;
      assert(f.save(c,false)==-EIO&&f.core.open()&&f.core.dirty()&&!f.led.ready()&&f.backend.writes.size()==1&&!f.service.receiving()&&!f.radio.sends);
      assert(f.coreJournal.config().same(old));const auto count=f.backend.writes.size();assert(f.commit.commit(30)<0&&f.backend.writes.size()==count);
      f.backend.failPrefix.clear();mt::SettingsJournal core;mt::NotificationJournal led;assert(!core.load(f.backend)&&!led.load(f.backend)&&core.config().same(old)&&led.config().heartbeatDisabled()==!committed);}
    puts("PASS notification first-write EIO is failure even when durable; core stays old and poisoned LED prevents retry");
  }
  {
    for(bool committed:{false,true}){Fixture f;const auto old=f.service.config;const auto c=f.candidate();f.backend.failPrefix="mtbase/state";f.backend.committed=committed;
      assert(f.save(c,false)==-EIO&&f.core.open()&&!f.coreJournal.ready()&&f.coreJournal.config().same(old)&&!f.led.config().heartbeatDisabled()&&!f.led.dirty());
      assert(f.backend.writes.size()==2&&!f.service.receiving()&&!f.radio.sends);const auto count=f.backend.writes.size();assert(f.commit.commit(30)<0&&f.backend.writes.size()==count);
      f.backend.failPrefix.clear();mt::SettingsJournal core;mt::NotificationJournal led;assert(!core.load(f.backend)&&!led.load(f.backend)&&core.config().same(committed?c:old)&&!led.config().heartbeatDisabled());}
    puts("PASS second core-write EIO never reports compound success; persisted LED and possible core commit are explicitly non-atomic");
  }
  {
    Fixture f;const auto old=f.service.config;const auto rows=f.backend.files;assert(f.save(f.candidate(),false,10,false)==-ENOTSUP&&f.backend.files==rows&&f.backend.writes.empty()&&f.service.config.same(old));
    auto bad=f.candidate();bad.bytes[0]^=1;assert(f.save(bad,false)==-EINVAL&&f.backend.writes.empty()&&f.service.config.same(old));
    f.radio.busy_=true;assert(f.save(f.candidate(),false)==-EBUSY&&f.backend.writes.empty()&&!f.core.open()&&f.led.config().heartbeatDisabled());
    puts("PASS missing LED capability, invalid core and busy accepted radio work refuse before journal writes");
  }
  {
    Fixture f;assert(!f.core.begin(10)&&!f.save(f.service.config,false,20)&&f.core.open()&&!f.core.dirty()&&f.led.dirty()&&f.backend.writes.empty());
    assert(!f.commit.commit(30)&&!f.core.open()&&f.backend.writes.size()==1&&f.backend.writes[0].find("mtnotify/")==0);
    puts("PASS heartbeat-only change within an open batch remains deferred without fabricating a core dirty write");
  }
  puts("DEVICE_HEARTBEAT_COMMIT_GROUPS 7");
}
