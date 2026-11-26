#include "network/WifiStationEdit.h"
#include "SettingsEdit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static constexpr uint32_t Self=0x12345678;
using Bytes=std::vector<uint8_t>;
struct Backend final:mt::SettingsBackend {
  std::map<std::string,Bytes> files;
  unsigned writes=0;
  std::string failAfterCommitKey;
  int read(const char *key,uint8_t *out,size_t cap) override {
    auto it=files.find(key); if(it==files.end()) return -ENOENT;
    if(it->second.size()>cap) return -E2BIG;
    memcpy(out,it->second.data(),it->second.size()); return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t n) override {
    ++writes; files[key]=Bytes(data,data+n);
    return key==failAfterCommitKey ? -EIO : 0;
  }
};
struct Radio final:mt::Radio {
  int state=0; bool ready()override{return true;}
  int start()override{state=1;return 0;} int restore()override{state=1;return 0;}
  int stop()override{state=0;return 0;} int health()override{return state;}
  bool busy()override{return false;} uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t*,size_t)override{return 0;} bool completion(int&)override{return false;}
};
struct Fixture {
  Radio radio; Backend backend; mt::test::KnownChannelFixtureClock clock;
  mt::Service service; mt::SettingsJournal coreJournal; mt::SettingsEdit core;
  mt::network::WifiStationJournal wifiJournal; mt::network::WifiStationEdit wifi;
  Fixture():service(radio,true,Self,127,true,false),core(service,coreJournal,backend),
            wifi(service,wifiJournal,backend) {
    clock.bind(service);
    assert(coreJournal.load(backend)==0 && wifiJournal.load(backend)==0 && wifi.load());
    auto initial=coreJournal.config(); initial.bytes[4]=1; initial.setTxEnabled(true);
    assert(coreJournal.save(backend,initial)==0 && service.apply(initial));
    assert(service.start(false,0) && service.armEndpoint(true));
  }
};
static bool bytesEqual(const uint8_t *a,const uint8_t *b,size_t n){return memcmp(a,b,n)==0;}
static mt::network::WifiStationSettings station(bool enabled,const char *ssid,const char *psk){
  mt::network::WifiStationSettings s; assert(s.set(enabled,ssid,strlen(ssid),psk,strlen(psk))); assert(s.valid()); return s;
}
static void runWifiStationEditCases(){
  // Immediate save persists every record byte and preserves healthy service readiness.
  {
    Fixture f; const auto base=f.backend.files; const unsigned writes=f.backend.writes;
    const auto candidate=station(true,"A","B");
    assert(f.wifi.save(candidate,10,false)==0);
    assert(f.backend.writes==writes+1 && f.service.receiving() && f.service.endpointArmed());
    const auto &raw=f.backend.files.at("mtwifi/state0");
    assert(raw.size()==mt::network::WifiStationJournal::RecordSize);
    assert(bytesEqual(raw.data()+12,candidate.bytes,mt::network::WifiStationSettings::Size));
    for(const auto &entry:base) assert(f.backend.files.at(entry.first)==entry.second);
    mt::network::WifiStationJournal reboot; assert(reboot.load(f.backend)==0);
    assert(bytesEqual(reboot.config().bytes,candidate.bytes,mt::network::WifiStationSettings::Size));
  }
  // Stage inside the outer core batch; neither journal writes until its own commit.
  {
    Fixture f; const unsigned writes=f.backend.writes; const auto candidate=station(true,"A","B");
    assert(f.core.begin(20)==0);
    auto changed=f.service.config; assert(changed.setNodeInfoSeconds(7200)); assert(f.core.save(changed,21)==0);
    assert(f.wifi.save(candidate,22,f.core.open())==0 && f.wifi.dirty());
    assert(f.backend.writes==writes && f.backend.files.count("mtwifi/state0")==0);
    assert(f.core.commit(23)==0 && f.backend.writes==writes+1);
    assert(f.wifi.commit(24)==0 && f.backend.writes==writes+2);
    assert(!f.wifi.dirty() && f.service.receiving() && f.service.endpointArmed());
    const auto &raw=f.backend.files.at("mtwifi/state0");
    assert(raw.size()==mt::network::WifiStationJournal::RecordSize);
    assert(bytesEqual(raw.data()+12,candidate.bytes,mt::network::WifiStationSettings::Size));
  }
  // Backend commits bytes then returns EIO: no success/current update; journal poisons readiness.
  {
    Fixture f; const auto previous=f.wifi.config(); const auto candidate=station(true,"A","B");
    f.backend.failAfterCommitKey="mtwifi/state0";
    assert(f.wifi.save(candidate,30,false)==-EIO);
    assert(!f.wifi.ready() && !f.wifi.dirty());
    assert(bytesEqual(f.wifi.config().bytes,previous.bytes,mt::network::WifiStationSettings::Size));
    const auto &raw=f.backend.files.at("mtwifi/state0");
    assert(raw.size()==mt::network::WifiStationJournal::RecordSize);
    assert(bytesEqual(raw.data()+12,candidate.bytes,mt::network::WifiStationSettings::Size));
  }
  // Invalid records are inert; disabling an accepted station preserves its credential bytes.
  {
    Fixture f; auto enabled=station(true,"A","B"); assert(f.wifi.save(enabled,40,false)==0);
    auto invalid=enabled; invalid.bytes[106]^=1; const unsigned writes=f.backend.writes;
    assert(f.wifi.save(invalid,41,false)==-EINVAL && f.backend.writes==writes);
    assert(bytesEqual(f.wifi.config().bytes,enabled.bytes,mt::network::WifiStationSettings::Size));
    auto disabled=enabled; assert(disabled.setEnabled(false));
    assert(f.wifi.save(disabled,42,false)==0);
    mt::network::WifiStationJournal reboot; assert(reboot.load(f.backend)==0);
    assert(bytesEqual(reboot.config().bytes,disabled.bytes,mt::network::WifiStationSettings::Size));
    assert(!reboot.config().enabled() && reboot.config().ssidLength()==1 && reboot.config().pskLength()==1);
  }
}
int main(){runWifiStationEditCases();}
