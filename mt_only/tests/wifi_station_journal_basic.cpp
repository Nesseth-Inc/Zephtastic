#include "network/WifiStationJournal.h"
#include <assert.h>
#include <errno.h>
#include <map>
#include <string>
#include <vector>
#include <string.h>
struct B:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> rows;
  int writes=0; std::vector<std::string> keys;
  int read(const char*k,uint8_t*out,size_t cap)override{keys.emplace_back(k);auto i=rows.find(k);if(i==rows.end())return -ENOENT;if(i->second.size()>cap)return -E2BIG;memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
  int write(const char*k,const uint8_t*d,size_t n)override{keys.emplace_back(k);++writes;rows[k]=std::vector<uint8_t>(d,d+n);return 0;}
};
int main(){
  B b;b.rows["mtbase/state0"]={1,2};b.rows["mtmod/state0"]={3,4};auto base=b.rows["mtbase/state0"];auto mod=b.rows["mtmod/state0"];
  mt::network::WifiStationJournal j;assert(j.load(b)==0&&j.ready()&&j.state()==mt::network::WifiStationJournal::State::Fresh&&j.activeSlot()==-1&&j.config().valid()&&!j.config().enabled()&&b.writes==0);
  mt::network::WifiStationSettings a;assert(a.set(true,"A",1,"B",1));assert(j.save(b,a)==0&&j.generation()==1&&j.activeSlot()==0);
  auto raw=b.rows["mtwifi/state0"];assert(raw.size()==124&&raw[0]=='Z'&&raw[1]=='T'&&raw[2]=='W'&&raw[3]==1&&raw[4]==1&&raw[8]==108&&raw[9]==0&&raw[10]==0&&raw[11]==0);
  assert(raw[12+106]==0x34&&raw[12+107]==0xb6&&raw[120]==0xf1&&raw[121]==0xcc&&raw[122]==0x9c&&raw[123]==0xb6);
  mt::network::WifiStationJournal reboot;assert(reboot.load(b)==0&&reboot.state()==mt::network::WifiStationJournal::State::Loaded&&reboot.generation()==1&&reboot.activeSlot()==0&&reboot.config().enabled());
  {
    auto ntp=reboot.config();
    assert(!*ntp.ntpServer() && ntp.encodedSize()==108);
    const char *server="192.0.2.1";
    assert(ntp.setNtp(server,strlen(server)) && ntp.valid() && ntp.encodedSize()==141);
    assert(!ntp.setNtp("-bad",4) && !ntp.setNtp("a..b",4) && !ntp.setNtp("x/y",3));
    B temp=b;
    mt::network::WifiStationJournal edit;
    assert(edit.load(temp)==0 && edit.save(temp,ntp)==0);
    auto &v2=temp.rows["mtwifi/state1"];
    assert(v2.size()==157 && v2[3]==2 && v2[8]==141);
    mt::network::WifiStationJournal restart;
    assert(restart.load(temp)==0 && !strcmp(restart.config().ntpServer(),server));
    assert(restart.save(temp,ntp)==0 && restart.generation()==2);
    auto cleared=restart.config();
    assert(cleared.setNtp(nullptr,0) && restart.save(temp,cleared)==0);
    assert(temp.rows["mtwifi/state0"].size()==124);
    auto corrupted=b;
    v2[12+108]^=1; // The outer checksum must cover the optional server too.
    corrupted.rows["mtwifi/state1"]=v2;
    assert(restart.load(corrupted)==0 && restart.generation()==1 && !*restart.config().ntpServer());
    assert(restart.state()==mt::network::WifiStationJournal::State::Recovered);
  }
  auto disabled=reboot.config();assert(disabled.setEnabled(false));assert(reboot.save(b,disabled)==0&&reboot.generation()==2&&reboot.activeSlot()==1&&b.rows["mtwifi/state1"].size()==124);
  b.rows["mtwifi/state0"].resize(50);mt::network::WifiStationJournal recovered;assert(recovered.load(b)==0&&recovered.state()==mt::network::WifiStationJournal::State::Recovered&&recovered.generation()==2&&!recovered.config().enabled());
  assert(b.rows["mtbase/state0"]==base&&b.rows["mtmod/state0"]==mod);
  for(const auto& k:b.keys)assert(k=="mtwifi/state0"||k=="mtwifi/state1");
}
