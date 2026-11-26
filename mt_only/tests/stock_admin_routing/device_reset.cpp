// SPDX-License-Identifier: GPL-3.0-only
#include "stock/admin/DeviceReset.h"
#include "pki/PkiStorage.h"
#include "pki/PsaPkiBackend.h"
extern "C" psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *,uint8_t *,size_t,size_t *out){
  *out=0;return PSA_ERROR_INSUFFICIENT_ENTROPY;
}
#include <array>
#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>

using namespace mt::pki;
namespace reset=mt::stock::admin::device_reset;
using Floor=reset::Floor;
struct Flash {
  std::array<uint8_t,Floor::PageBytes> page{};
  int budget=-1,readError=0;bool outsideBlank=true;
  Flash(){page.fill(255);}
  int read(size_t at,uint8_t *out,size_t n){
    assert(at+n<=page.size());if(readError)return readError;memcpy(out,page.data()+at,n);return 0;
  }
  int program(size_t at,const uint8_t *in,size_t n){
    assert(at+n<=page.size());
    for(unsigned i=0;i<n;++i){
      if(!budget)return -EIO;
      if(budget>0)--budget;
      assert((in[i]&page[at+i])==in[i]);page[at+i]&=in[i];
    }return 0;
  }
  int firstEnrollment(bool=false){return outsideBlank?0:-EEXIST;}
};
struct Store : mt::SettingsBackend{
  std::unordered_map<std::string,std::vector<uint8_t>> rows;
  int cut=-1,ops=0;
  int fault(){return ops++==cut?-EIO:0;}
  int read(const char *k,uint8_t *out,size_t cap)override{
    auto i=rows.find(k);if(i==rows.end())return -ENOENT;
    if(i->second.size()>cap)return -E2BIG;
    memcpy(out,i->second.data(),i->second.size());return int(i->second.size());
  }
  int write(const char *k,const uint8_t *data,size_t n)override{
    if(fault())return -EIO;rows[k]={data,data+n};return 0;
  }
  int eraseVerified(const char *k){if(fault())return -EIO;rows.erase(k);return 0;}
};
struct Settings: FreshProvisioningSettings {
  Store &s;explicit Settings(Store &s):s(s){}
  int begin()override{return 0;}void end()override{}
  int read(const char *k,uint8_t *o,size_t n)override{return s.read(k,o,n);}
  int persist(const char *k,const uint8_t *b,size_t n)override{return s.write(k,b,n);}
};
struct Context:ProvisioningContext,PkiStorageContext{
  bool tryEnter()override{return true;}void leave()override{}
  bool quiescent()const override{return true;}bool bootQuiescent()const override{return true;}
  uint64_t millis()const override{return 100;}
  bool candidateAllowed(const IdentityBinding&)const override{return true;}
  bool peerAllowed(const IdentityBinding&,const ProvisionPeer&)const override{return true;}
};
struct Entropy:EntropySource{
  uint8_t seed;explicit Entropy(uint8_t seed):seed(seed){}
  bool fill(uint8_t *out,size_t n)override{for(size_t i=0;i<n;++i)out[i]=uint8_t(seed+23*i);return true;}
};
int provision(Store &s,uint8_t seed,IdentityBinding &binding,const IdentityBinding *expected=nullptr){
  Context context;Entropy entropy(seed);Settings settings(s);
  PkiFreshProvisioning p(settings,context,entropy);int rc=p.stage(binding);
  if(rc)return rc;
  if(expected&&(binding.node!=expected->node||memcmp(binding.publicKey,expected->publicKey,32)))return -EXDEV;
  return p.confirm(binding);
}
int verify(Store &s,IdentityBinding &binding){
  Context context;Entropy entropy(11);Settings settings(s);PkiStorage storage(settings,s,context,entropy);
  int rc=storage.prepare();if(rc)return rc;
  return storage.preparedBinding(binding)?reset::freshBinding(s,binding):-EACCES;
}
int boot(Flash &flash,Store &s,bool &online){
  online=false;Floor::Head h;int rc=reset::inspect(flash,s,h);if(rc)return rc;
  if(h.state!=Floor::State::Unused){
    rc=reset::ready(s,h);
    if(rc&&h.state==Floor::State::Selected&&(rc==-ENOENT||rc==-ESTALE)){
      return reset::rebuild(s,h,[&](){int a=s.eraseVerified("bt/keys/01");return a?a:s.eraseVerified("mtble/gatt/42");},
        [&](){return s.eraseVerified("file:/static/rangetest.csv");},
        [&](const IdentityBinding &expected){IdentityBinding copy;return provision(s,135,copy,&expected);});
    }
    if(rc)return rc;
  }
  IdentityBinding actual;if((rc=verify(s,actual)))return rc;
  if(h.state!=Floor::State::Unused){
    if(memcmp(h.newKey,actual.publicKey,32))return -EACCES;
    if(h.state!=Floor::State::Active)return Floor::activate(flash,h,actual.publicKey);
    if((rc=s.eraseVerified(reset::RequestKey)))return rc;
  }
  online=true;return 0;
}
void untilOnline(Flash &flash,Store &s){
  bool online=false;for(unsigned i=0;!online&&i<5;++i)assert(boot(flash,s,online)==0);
  assert(online);
}
int main(){
  Flash f;Store original;IdentityBinding first;assert(!provision(original,21,first));
  original.rows["bt/keys/01"]={1};original.rows["mtble/gatt/42"]={1};
  original.rows["file:/static/rangetest.csv"]={1};original.rows["mtba/b15"]={1};
  original.rows["mtba/m0"]={1};original.rows["mtbase/state1"]={1};
  original.rows["other/setting"]={10};original.rows["mtxsensor/bme680"]={1,2,3};
  bool attempted=false,online=false;Entropy next(65);
  Flash occupied;occupied.outsideBlank=false;Store never=original;
  assert(reset::begin(occupied,never,first,next,attempted)==-EEXIST&&!attempted&&never.rows==original.rows);
  const auto unchanged=f.page;f.readError=-EIO;
  assert(reset::begin(f,never,first,next,attempted)==-EIO&&!attempted&&unchanged==f.page);f.readError=0;
  Store staged=original;assert(!reset::begin(f,staged,first,next,attempted)&&attempted);
  Floor::Head h;assert(!Floor::inspect(f,h)&&h.state==Floor::State::Selected);
  Flash selected=f;
  assert(Floor::enrollmentHeader(selected.page.data()));
  const uint8_t unrelated[8]={1,0,2,0,3,0,4,0};assert(!Floor::enrollmentHeader(unrelated));
  {Flash partial;Store data=original;partial.budget=12;
   assert(reset::begin(partial,data,first,next,attempted)<0);partial.budget=-1;
   const auto page=partial.page;partial.outsideBlank=false;
   assert(boot(partial,data,online)==-EEXIST&&!online&&partial.page==page);
   partial.outsideBlank=true;data.rows.erase(PkiFreshProvisioning::RootKey);
   assert(boot(partial,data,online)==-ENOENT&&!online&&partial.page==page);}
  for(unsigned cut=0;cut<=96;++cut){
    Flash broken;Store data=original;broken.budget=int(cut);
    auto rc=reset::begin(broken,data,first,next,attempted);if(cut<96)assert(rc<0);else assert(!rc);
    assert(Floor::enrollmentHeader(broken.page.data())==(cut!=0));
    broken.budget=-1;untilOnline(broken,data);IdentityBinding active;assert(!verify(data,active));
    // No changed floor at cut zero. All other cuts retire the old identity.
    assert((memcmp(first.publicKey,active.publicKey,32)!=0)==(cut!=0));
  }
  Store completed=staged;completed.ops=0;assert(!boot(f,completed,online)&&!online);
  const int eraseSteps=completed.ops;assert(eraseSteps>80);
  for(int cut=0;cut<eraseSteps;++cut){
    Store data=staged;Flash blank=selected;data.ops=0;data.cut=cut;
    assert(boot(blank,data,online)<0&&!online);data.cut=-1;untilOnline(blank,data);
    IdentityBinding actual;assert(!verify(data,actual)&&!memcmp(actual.publicKey,h.newKey,32));
    assert(!data.rows.count("mtxsensor/bme680"));
  }
  Store built=completed;
  for(unsigned cut=0;cut<=16;++cut){
    Flash partial=selected;Store data=built;partial.budget=int(cut);
    int rc=boot(partial,data,online);assert(!online&&((rc!=0)==(cut<16)));
    partial.budget=-1;untilOnline(partial,data);
  }
  untilOnline(f,completed);
  assert(!completed.rows.count("bt/keys/01")&&!completed.rows.count("mtble/gatt/42")&&
         !completed.rows.count("mtba/b15")&&!completed.rows.count("mtba/m0")&&
         !completed.rows.count("mtxsensor/bme680")&&!completed.rows.count("file:/static/rangetest.csv")&&!completed.rows.count("mtbase/state1")&&completed.rows.count("other/setting"));
  IdentityBinding second;assert(!verify(completed,second)&&memcmp(first.publicKey,second.publicKey,32));
  Store rollback=original;assert(boot(f,rollback,online)<0&&!online&&rollback.rows==original.rows);
  rollback=staged;assert(boot(f,rollback,online)<0&&!online&&rollback.rows==staged.rows);
  Store legacy=staged;legacy.rows["mtpki/ledger0"]={1};auto backup=legacy.rows;
  assert(boot(selected,legacy,online)==-EXDEV&&!online&&backup==legacy.rows);
  Flash corrupt=f;corrupt.page[0]=0;assert(boot(corrupt,completed,online)<0&&!online);
  Flash unreadable=f;unreadable.readError=-EIO;assert(boot(unreadable,completed,online)==-EIO&&!online);
  auto secondSnapshot=completed;Entropy retired(21);assert(reset::begin(f,completed,second,retired,attempted)==-EEXIST&&!attempted);
  Entropy third(111);assert(!reset::begin(f,completed,second,third,attempted));untilOnline(f,completed);
  assert(boot(f,secondSnapshot,online)<0&&!online&&boot(f,original,online)<0&&!online);
  printf("PASS two full resets, original and previous NVS restores denied, real fresh PKI boot verified, 97 retirement/selection boundaries, 17 activation boundaries, %d interrupted settings writes and removals; bonds, backup and CSV cleared\n",eraseSteps);
}
