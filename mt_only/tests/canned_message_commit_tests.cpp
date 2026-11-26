// SPDX-License-Identifier: GPL-3.0-only
#include "modules/CannedMessageCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace mt;
using namespace mt::modules;

struct Backend final:SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> rows;
  unsigned writes=0;int writeError=0;bool corruptReadback=false;
  int read(const char *key,uint8_t *out,size_t capacity)override{
    auto it=rows.find(key);if(it==rows.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());
    if(corruptReadback&&writes)out[0]^=1;
    return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t size)override{
    ++writes;if(writeError)return writeError;rows[key]={data,data+size};return 0;
  }
};

struct TestRadio final:Radio {
  int state=0,startError=0,pauseError=0,healthError=0;bool busyNow=false;
  unsigned starts=0,stops=0,pauses=0,sends=0;
  bool ready()override{return true;}
  int start()override{++starts;if(startError)return startError;state=1;return 0;}
  int stop()override{++stops;state=0;return 0;}
  int restore()override{state=1;return 0;}
  int stopForStorage()override{++pauses;if(pauseError)return pauseError;state=0;return 0;}
  int health()override{return healthError?healthError:state;}
  bool busy()override{return busyNow;}
  uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return 0;}
  bool completion(int &)override{return false;}
};

struct Fixture {
  TestRadio radio;Backend backend;CannedMessageJournal journal;test::KnownChannelFixtureClock clock;
  Service service;
  explicit Fixture(bool armed=true):service(radio,true,0x12345678,127,true,false){
    clock.bind(service);
    Config config;config.bytes[4]=1;config.setTxEnabled(true);auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(armed));
    assert(journal.load(backend)==0&&journal.ready());
  }
  int save(const CannedMessageSettings &value,uint32_t now=10){
    return CannedMessageCommit::save(service,radio,journal,backend,value,now);
  }
};

static CannedMessageSettings named(const char *value){
  CannedMessageSettings out;assert(out.assign(value,strlen(value))==CannedMessageSettings::AssignResult::Changed);return out;
}
static void pass(const char *label){std::printf("PASS %s\n",label);}

int main(){
  {
    for(bool armed:{false,true}){
      Fixture f(armed);const auto before=f.backend.rows;
      const auto candidate=named("Arrived|On my way|Māori");
      assert(!f.save(candidate)&&f.backend.writes==1&&f.journal.config().same(candidate));
      assert(f.service.receiving()&&f.service.endpointArmed()==armed&&f.radio.state==1&&!f.radio.sends);
      assert(f.backend.rows.size()==1&&f.backend.rows.count("mtcanned/state0")&&f.backend.rows!=before);
    }
    pass("successful saves persist only the canned namespace and restore prior RX with exact arm intent; no RF TX");
  }
  {
    Fixture f;const auto original=f.journal.config();
    assert(!f.save(original)&&f.backend.writes==0&&!f.radio.pauses&&f.service.receiving()&&f.service.endpointArmed());
    CannedMessageSettings malformed;memset(malformed.messages,'x',sizeof(malformed.messages));
    assert(f.save(malformed)==-EINVAL&&f.backend.writes==0&&!f.radio.pauses&&f.service.receiving());
    f.radio.busyNow=true;assert(f.save(named("busy"))==-EBUSY&&f.backend.writes==0&&!f.radio.pauses&&f.service.receiving());
    f.radio.busyNow=false;
    const uint8_t textData[]={0x08,0x01,0x12,0x02,'h','i',0x48,0};uint8_t frame[255]{};size_t frameSize=0;
    assert(f.service.encodePublicMessage(0xffffffffu,301,textData,sizeof(textData),frame,sizeof(frame),frameSize));
    assert(!f.service.submitMessage(0,frame,frameSize,true,10)&&!f.service.storageStopSafe());
    const unsigned pauses=f.radio.pauses;
    assert(f.save(named("queued"))==-EBUSY&&f.backend.writes==0&&f.radio.pauses==pauses&&f.service.receiving()&&f.service.endpointArmed());
    pass("unchanged, malformed, native-busy, and queued-work saves are inert/rejected before storage pause");
  }
  {
    Fixture f;f.radio.pauseError=-EAGAIN;
    assert(f.save(named("raced CAD"))==-EBUSY&&f.backend.writes==0&&f.radio.pauses==1&&
      f.service.receiving()&&f.service.endpointArmed()&&!f.service.faulted());
    pass("race-time CAD busy remains clean EBUSY with original RX and arm retained");
  }
  {
    for(int error:{-ENODEV,-ESHUTDOWN}){
      Fixture f;f.radio.pauseError=error;
      assert(f.save(named("stop failed"))==-EIO&&f.backend.writes==0&&f.service.faulted());
    }
    pass("hard native stop failures are interface errors and never write canned state");
  }
  {
    for(int error:{-EIO,-ENOSPC,-ENOMEM,-E2BIG,-EBUSY}){
      Fixture f;const auto candidate=named("backend fail");f.backend.writeError=error;
      assert(f.save(candidate)==-EIO&&f.backend.writes==1&&f.journal.state()==CannedMessageJournal::State::IoError);
      assert(f.service.receiving()&&f.service.endpointArmed()&&f.radio.state==1&&!f.radio.sends);
      assert(f.save(candidate)==-EACCES&&f.backend.writes==1);
    }
    Fixture readback;readback.backend.corruptReadback=true;
    assert(readback.save(named("readback uncertain"))==-EIO&&readback.journal.state()==CannedMessageJournal::State::IoError);
    assert(readback.service.receiving()&&readback.service.endpointArmed()&&readback.radio.state==1);
    pass("backend errors and uncertain readback poison the journal but still restore previous RX/arm");
  }
  {
    Fixture f;const auto candidate=named("persist then restore fails");f.radio.startError=-EIO;
    assert(f.save(candidate)==-EIO&&f.backend.writes==1&&f.journal.config().same(candidate));
    assert(!f.service.receiving()&&!f.service.endpointArmed()&&f.service.faulted());
    pass("verified persistence is not reported as success if RX restoration fails");
  }
  {
    Fixture f;f.backend.rows["mtbase/state0"]={1,2,3};f.backend.rows["mtmod/state1"]={4,5};
    f.backend.rows["mtextra/state0"]={6,7};f.backend.rows["mtpki/ledger0"]={8,9};
    const auto protectedRows=f.backend.rows;assert(!f.save(named("no unrelated rewrites")));
    for(const auto &entry:protectedRows)assert(f.backend.rows.at(entry.first)==entry.second);
    for(const auto &entry:f.backend.rows)assert(entry.first=="mtcanned/state0"||entry.first=="mtbase/state0"||
      entry.first=="mtmod/state1"||entry.first=="mtextra/state0"||entry.first=="mtpki/ledger0");
    pass("composition helper modifies only mtcanned slots and preserves unrelated setting/PKI rows");
  }
}
