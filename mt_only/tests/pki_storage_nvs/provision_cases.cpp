// SPDX-License-Identifier: GPL-3.0-only
// Actual mounted NVS/settings implementation, synthetic NOR and public scalars.
#include "pki/NvsProvisioningSettings.h"
#include "MtSignature.h"
#include "monocypher.h"
#include "bridge.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace mt::pki;
struct Context:ProvisioningContext,PkiStorageContext {
  unsigned depth=0;bool permit=true;uint64_t now=100;
  bool tryEnter()override{++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool quiescent()const override{return depth&&permit;}
  bool bootQuiescent()const override{return quiescent();}
  uint64_t millis()const override{return now;}
  bool candidateAllowed(const IdentityBinding &)const override{return true;}
  bool peerAllowed(const IdentityBinding &,const ProvisionPeer &)const override{return true;}
};
struct Entropy:EntropySource {
  unsigned calls=0;
  bool fill(uint8_t *out,size_t n)override{++calls;for(size_t i=0;i<n;++i)out[i]=uint8_t(19+23*i);return true;}
};
static ProvisionPeer peer(){
  uint8_t scalar[32];for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(71+23*i);
  scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;
  ProvisionPeer p;crypto_x25519_public_key(p.publicKey,scalar);p.node=bench::MtSignature::nodeId(p.publicKey);
  eraseIdentityBytes(scalar,32);return p;
}
static constexpr const char *names[]={"mtbase/config","mtbase/state0","mtbase/state1","mtbase/nodes0","mtbase/nodes1"};
static constexpr size_t sizes[]={80,128,128,1492,1492};
static void populate(){mt::ZephyrSettings s;uint8_t bytes[1492];for(unsigned i=0;i<5;++i){memset(bytes,int(i+31),sizes[i]);assert(!s.write(names[i],bytes,sizes[i]));}}
static void protectedUnchanged(){
  // Direct nvs_read only: this observation must never invoke settings repairs.
  uint8_t bytes[1492];const auto writes=model_writes();
  for(unsigned i=0;i<5;++i){assert(model_get(uint16_t(0xc001+i),bytes,sizeof(bytes))==ssize_t(sizes[i]));for(size_t j=0;j<sizes[i];++j)assert(bytes[j]==i+31);}
  assert(writes==model_writes());
}
static void observe(const IdentityBinding &expected,bool requireComplete){
  NvsBootSettings boot;mt::ZephyrSettings writer;Context context;Entropy entropy;
  PkiStorage storage(boot,writer,context,entropy);unsigned writes=model_writes();const int rc=storage.prepare();
  assert(model_writes()==writes&&!entropy.calls);
  if(!rc){IdentityBinding b;uint32_t p=0;assert(storage.preparedBinding(b)&&b.node==expected.node&&!memcmp(b.publicKey,expected.publicKey,32));assert(storage.preparedPeer(p)&&p==peer().node);}
  else {assert(!requireComplete);assert(rc==-ENOENT||rc==-ENODATA||rc==-EILSEQ||rc==-EIO);assert(!storage.runtimeIdentity());}
  protectedUnchanged();
}
static void run(unsigned sectors){
  model_reset(sectors);populate();
  {NvsProvisioningSettings settings;std::array<uint8_t,PkiLedger::RecordSize> value{};
    assert(settings.persist("mtbase/state0",value.data(),128)==-EACCES);
    assert(settings.persist("mtidentity/state1",value.data(),84)==-EACCES);
    assert(settings.persist("mtpki/ledger0",value.data(),84)==-EACCES);
    assert(!settings.begin());assert(settings.begin()==-EBUSY);
    assert(settings.persist("mtpki/ledger0",value.data(),value.size())==-EACCES);settings.end();
    assert(settings.read("mtpki/ledger0",value.data(),value.size())==-EACCES);protectedUnchanged();}
  printf("PASS %usector fixed mounted provisioning adapter rejects foreign namespace, size and held-snapshot writes\n",sectors);
  model_snapshot();unsigned events=0;IdentityBinding candidate;
  {NvsProvisioningSettings settings;Context context;Entropy entropy;PkiProvisioning core(settings,context,entropy);
    const auto writes=model_writes();assert(!core.stage(candidate)&&entropy.calls==1&&writes==model_writes());
    model_fail(0,0);assert(!core.commit(candidate,peer()));events=model_events();assert(events>0);
    assert(core.state()==ProvisionState::Committed);ProvisionStatus status;assert(!core.inspect(status)&&status.stored==ProvisionStored::Complete&&status.hasBinding&&status.hasPeer);}
  model_remount();observe(candidate,true);
  printf("PASS %usector actual NVS provisioning commits ledger domain, pin and identity then remounts exact tuple with normal records preserved\n",sectors);
  unsigned faults=0,complete=0;
  for(unsigned event=1;event<=events;++event)for(int phase=0;phase<3;++phase){
    model_restore();IdentityBinding expected;
    {NvsProvisioningSettings settings;Context context;Entropy entropy;PkiProvisioning core(settings,context,entropy);
      assert(!core.stage(expected));model_fail(event,phase);const int rc=core.commit(expected,peer());
      if(rc){assert(core.state()==ProvisionState::Frozen);const auto writes=model_writes();model_no_fault();assert(core.commit(expected,peer())<0&&core.stage(expected)<0);assert(writes==model_writes()&&entropy.calls==1);}
      else {assert(core.state()==ProvisionState::Committed);++complete;}}
    model_no_fault();model_remount();observe(expected,false);++faults;
  }
  printf("PASS %usector actual provisioning raw-flash faults: %u before/partial/after; %u internally hidden close-marker errors require exact readback; no retry or normal-record damage\n",sectors,faults,complete);
}
int main(){run(4);run(8);}
