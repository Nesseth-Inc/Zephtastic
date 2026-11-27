// SPDX-License-Identifier: GPL-3.0-only
#include "stock/admin/FactoryConfigReset.h"
#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>
namespace reset=mt::stock::admin::factory;
using Bytes=std::vector<uint8_t>;
// This fixture is built with normal and short enums against the system C++
// library. The ordered tree shares a color enum across that ABI boundary.
// The settings backend needs only key lookup, erase, copy and equality.
using Rows=std::unordered_map<std::string,Bytes>;
struct Store {
  Rows rows;
  int failAt=-1,calls=0;
  int read(const char *name,uint8_t *bytes,size_t size){
    auto i=rows.find(name);if(i==rows.end())return -ENOENT;
    if(i->second.size()>size)return -E2BIG;
    memcpy(bytes,i->second.data(),i->second.size());return int(i->second.size());
  }
  int write(const char *name,const uint8_t *value,size_t n){rows[name]=Bytes(value,value+n);return 0;}
  int eraseVerified(const char *name){
    if(calls++==failAt)return -EIO;
    rows.erase(name);return 0;
  }
};
int main(){
  const uint32_t node=42;uint8_t key[32]={9};
  Store seed;reset::Phase phase;assert(!reset::inspect(seed,node,key,phase)&&phase==reset::Phase::Idle);
  Rows retained;
  for(const char *name:{"mtidentity/state0","mtpki/fresh0","mtpki/contacts0","mtpki/nonces1","mtpki/ledger0",
                        "bt/keys/x","mtble/gatt/x","mtba/m0","mtba/a00","mtba/b15","mtba/ri","unrelated/value"})retained[name]={99};
  seed.rows=retained;unsigned eligible=0;
  assert(!reset::erasePreferences([&](const char *name){
    assert(!seed.rows.count(name));seed.rows[name]={1,2,3};++eligible;return 0;
  }));
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
  assert(eligible==50&&seed.rows.count("mttx/history0")&&seed.rows.count("mttx/history1"));
#else
  assert(eligible==48);
#endif
  assert(seed.rows.count("mthop/state")); // also retire history made by a previous feature-enabled image
  assert(seed.rows.count("mtxsensor/bme680")&&seed.rows.count("mtsat/epoch")&&seed.rows.count("mtxsensor/sen5hist")&&seed.rows.count("mtxsensor/sen6hist")&&seed.rows.count("mtxsensor/nau")&&seed.rows.count("mtxsensor/as3935")&&seed.rows.count("mtxsensor/sen5")&&seed.rows.count("mtxsensor/sen6")&&seed.rows.count("mtway/history")&&seed.rows.count("mtba/b15")&&seed.rows.count("mtbase/config")&&seed.rows.count("mtgames/scores1"));
  assert(!reset::mark(seed,node,key,reset::Phase::RetireContacts));
  assert(reset::finish(seed,node,key,[]{return 0;})==-EINVAL);
  assert(reset::retireAndAdvance(seed,node+1,key,[]{assert(false);return 0;})==-EILSEQ);
  assert(reset::retireAndAdvance(seed,node,key,[]{return -EBUSY;})==-EBUSY);
  assert(!reset::inspect(seed,node,key,phase)&&phase==reset::Phase::RetireContacts);
  bool retired=false;assert(!reset::retireAndAdvance(seed,node,key,[&](){retired=true;return 0;}));
  assert(retired);
  Store fileFail=seed;assert(reset::finish(fileFail,node,key,[]{return -EIO;})==-EIO);
  assert(!reset::inspect(fileFail,node,key,phase)&&phase==reset::Phase::ErasePreferences);
  assert(!reset::finish(fileFail,node,key,[]{return 0;})&&fileFail.rows==retained);
  for(unsigned cut=0;cut<=eligible;++cut){
    Store trial=seed;trial.failAt=int(cut);assert(reset::finish(trial,node,key,[]{return 0;})==-EIO);
    assert(!reset::inspect(trial,node,key,phase)&&phase==reset::Phase::ErasePreferences);
    trial.failAt=-1;assert(!reset::finish(trial,node,key,[]{return 0;}));
    assert(trial.rows==retained&&!reset::inspect(trial,node,key,phase)&&phase==reset::Phase::Idle);
  }
  auto valid=seed.rows[reset::Intent];
  for(unsigned i=0;i<valid.size();++i){
    Store bad=seed;bad.rows[reset::Intent][i]^=1;
    assert(reset::finish(bad,node,key,[]{assert(false);return 0;})<0&&bad.calls==0);
  }
  printf("PASS config reset: %u owned preference keys, %u interrupted erasures resumed, wrong domain/corrupt marker, failed contact/file, identity/nonce/bonds/FLASH backups retained\n",eligible,eligible+1);
}
