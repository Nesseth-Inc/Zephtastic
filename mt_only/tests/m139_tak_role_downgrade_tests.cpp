// SPDX-License-Identifier: GPL-3.0-only
// Stored optional-role downgrade recovery. RAM-only backend and mock radio.
#include "NormalTxStartup.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <iostream>

namespace {
constexpr uint32_t Node=0x12345678u;
struct Memory final : mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> values; unsigned writes=0;
  int read(const char *key,uint8_t *out,size_t cap) override {
    auto i=values.find(key); if(i==values.end()) return -ENOENT;
    if(i->second.size()>cap)return -E2BIG;
    memcpy(out,i->second.data(),i->second.size());return int(i->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t n) override {
    ++writes;values[key]=std::vector<uint8_t>(p,p+n);return 0;
  }
};
struct Radio final : mt::Radio {
  unsigned configured=0,starts=0,stops=0,sends=0;int state=0;
  bool ready()override{return true;}
  int configure(const mt::Config &c)override{++configured;return c.valid()?0:-EINVAL;}
  int start()override{++starts;state=1;return 0;}
  int restore()override{state=1;return 0;}
  int stop()override{++stops;state=0;return 0;}
  int health()override{return state;}
  bool busy()override{return false;}
  uint32_t airtime(size_t)override{return 10;}
  int send(uint8_t *,size_t)override{++sends;return 0;}
  bool completion(int &r)override{r=0;return true;}
};
uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
std::vector<uint8_t> record(const mt::Config &c,uint32_t generation=1){
  std::vector<uint8_t> r(mt::SettingsJournal::RecordSize,0);r[0]='Z';r[1]='T';r[2]='J';r[3]=6;
  put32(r.data()+4,generation);r[8]=uint8_t(mt::Config::Size);r[9]=uint8_t(mt::Config::Size>>8);
  memcpy(r.data()+12,c.bytes,mt::Config::Size);put32(r.data()+r.size()-4,crc32(r.data(),r.size()-4));return r;
}
mt::Config provisioned(uint32_t role=0){
  mt::Config c; c.bytes[4]=1; auto n=c.node();n.setPublic();c.node(n);
  assert(c.setDeviceRole(role,0));c.setOwnerUnmessagable(true);
  assert(c.valid()&&c.provisioned()&&!c.txIntentSpecified());return c;
}
mt::Config storedRole10(){
  auto c=provisioned();c.bytes[107]=10;c.bytes[108]=0;c.seal();assert(c.valid()&&c.role()==10);return c;
}
void sameExceptRoleAndCrc(const mt::Config &a,const mt::Config &b){
  for(size_t i=0;i<mt::Config::Size;++i)
    if(i!=107&&i!=108&&i!=mt::Config::Size-2&&i!=mt::Config::Size-1)assert(a.bytes[i]==b.bytes[i]);
}
void pass(const char *s){std::cout<<"PASS "<<s<<"\n";}
}

int main(){
  const bool compiled=mt::DeviceRolePolicy::supportedRole(10);
  {
    auto c=storedRole10();mt::Config decoded;
    assert(mt::Config::decode(c.bytes,mt::Config::Size,decoded));
    assert(decoded.same(c)&&decoded.role()==10&&decoded.ownerUnmessagable());
    Memory m;m.values["mtbase/state0"]=record(c);mt::SettingsJournal j;
    assert(j.load(m)==0&&j.ready()&&j.config().same(c)&&j.config().role()==10&&m.writes==0);
    auto legacy=c;legacy.bytes[2]=18;legacy.bytes[5]=0;legacy.seal();
    mt::Config rejected;assert(!mt::Config::decode(legacy.bytes,mt::Config::Size,rejected));
    Memory old;old.values["mtbase/state0"]=record(legacy);mt::SettingsJournal oldJournal;
    assert(oldJournal.load(old)<0&&!oldJournal.ready()&&old.writes==0);
    pass("v19 stored role 10 loads read-only in either build; v18 role 10 is rejected");
  }
  {
    auto c=provisioned();const bool selected=c.setDeviceRole(10,0);
    assert(selected==compiled);
    if(selected)assert(c.role()==10&&c.valid());
    auto router=provisioned(2);assert(!router.setDeviceRole(2,4));
    pass("new role selection follows the feature gate and existing invalid-role constraints");
  }
  {
    auto c=storedRole10();Memory m;m.values["mtbase/state0"]=record(c);
    mt::SettingsJournal j;assert(j.load(m)==0);const auto writes=m.writes;
    assert(mt::NormalTxStartup::migrate(j,m,true)==0);
    assert(m.writes==writes+(compiled?1u:0u)&&j.config().role()==10);
    if(!compiled)assert(!j.config().txIntentSpecified());
    else assert(j.config().txIntent()==mt::Config::TxIntent::Enabled);
    pass("startup TX-default migration is skipped for an uncompiled stored role");
  }
  {
    auto c=storedRole10();Radio radio;mt::Service service(radio,true,Node,81,true,false);
    assert(service.apply(c));
    if(!compiled)assert(!service.start(false,99)&&!service.receiving()&&!service.endpointArmed()&&
                        !radio.configured&&!radio.starts&&!radio.sends);
    const bool activated=mt::NormalTxStartup::activate(service,true,true,100);
    assert(activated);
    if(!compiled){
      assert(!service.receiving()&&!service.endpointArmed()&&!service.faulted());
      assert(!radio.configured&&!radio.starts&&!radio.sends);
    } else {
      assert(service.receiving()&&service.endpointArmed()&&!service.faulted());
      assert(radio.configured==1&&radio.starts==1);
      service.stop();
    }
    pass("normal startup never starts the radio for an unsupported persisted role");
  }
  {
    auto c=storedRole10();Memory m;m.values["mtbase/state0"]=record(c);mt::SettingsJournal j;
    assert(j.load(m)==0);const auto original=j.config();const auto writes=m.writes;
    auto supported=j.config();assert(supported.setDeviceRole(0,0));sameExceptRoleAndCrc(original,supported);
    assert(j.save(m,supported)==0&&m.writes==writes+1);
    mt::SettingsJournal reboot;assert(reboot.load(m)==0&&reboot.config().same(supported));
    assert(reboot.config().role()==0&&reboot.config().ownerUnmessagable()&&reboot.config().provisioned());
    sameExceptRoleAndCrc(original,reboot.config());
    Radio radio;mt::Service recovered(radio,true,Node,81,true,false);assert(recovered.apply(reboot.config()));
    assert(mt::NormalTxStartup::activate(recovered,true,true,200));
    assert(recovered.receiving()&&!recovered.faulted());
    if(!compiled)assert(recovered.endpointArmed());
    pass("explicit supported-role save and reload recovers the profile without changing other settings");
  }
}
