// SPDX-License-Identifier: GPL-3.0-only
#include "bridge.h"
#include "pki/NvsProvisioningSettings.h"
#include "stock/admin/DeviceReset.h"
#include "stock/admin/DeviceResetBonds.h"
#include <array>
#include <cassert>
#include <cstdio>

using namespace mt::pki;
namespace reset=mt::stock::admin::device_reset;
struct Context:ProvisioningContext,PkiStorageContext {
    unsigned depth=0;
    bool tryEnter()override{++depth;return true;}
    void leave()override{assert(depth);--depth;}
    bool quiescent()const override{return depth!=0;}
    bool bootQuiescent()const override{return quiescent();}
    uint64_t millis()const override{return 100;}
    bool candidateAllowed(const IdentityBinding &)const override{return true;}
    bool peerAllowed(const IdentityBinding &,const ProvisionPeer &)const override{return false;}
};
struct Entropy:EntropySource {
    uint8_t seed=21;
    bool fill(uint8_t *out,size_t n)override{
        for(size_t i=0;i<n;++i)out[i]=uint8_t(seed+23*i);
        return true;
    }
};
struct Flash {
    std::array<uint8_t,reset::Floor::PageBytes> bytes{};
    Flash(){bytes.fill(255);}
    int read(size_t at,uint8_t *out,size_t n){
        assert(at+n<=bytes.size());memcpy(out,bytes.data()+at,n);return 0;
    }
    int program(size_t at,const uint8_t *in,size_t n){
        assert(at+n<=bytes.size());
        for(size_t i=0;i<n;++i){assert((bytes[at+i]&in[i])==in[i]);bytes[at+i]&=in[i];}
        return 0;
    }
    int firstEnrollment(bool=false){return 0;}
};
int main(){
    model_reset(4);
    mt::ZephyrSettings settings;NvsProvisioningSettings setup;NvsBootSettings boot;
    Context ctx;Entropy entropy;IdentityBinding original;
    PkiFreshProvisioning genesis(setup,ctx,entropy);
    assert(!genesis.stage(original)&&!genesis.confirm(original));
    const uint8_t one=1;
    const char *const owned[]={"bt/keys/00","bt/ccc/01","mtble/gatt/01","mtba/m0","mtba/a15","mtway/history"};
    for(const char *key:owned)assert(!settings.write(key,&one,1));
    for(unsigned i=0;i<34;++i){
        char key[20];snprintf(key,sizeof(key),"bt/ccc/test%02u",i);
        assert(!settings.write(key,&one,1));
    }
    assert(!settings.write("bt-other/alpha",&one,1)&&!settings.write("mtble/gatt-old/01",&one,1));
    model_snapshot();
    Flash flash;bool attempted=false;
    entropy.seed=65;
    assert(reset::begin(flash,settings,original,entropy,attempted)==0&&attempted);
    model_remount();
    reset::Floor::Head head;
    assert(!reset::inspect(flash,settings,head)&&head.state==reset::Floor::State::Selected);
    auto rebuild=[&](){return reset::rebuild(settings,head,[&](){
        int rc=reset::eraseSubtree(settings,"bt");
        return rc?rc:reset::eraseSubtree(settings,"mtble/gatt");
    },[](){return 0;},[&](const IdentityBinding &expected){
        PkiFreshProvisioning installer(setup,ctx,entropy);IdentityBinding staged;
        const int rc=installer.stage(staged);if(rc)return rc;
        return memcmp(staged.publicKey,expected.publicKey,32)?-EXDEV:installer.confirm(expected);
    });};
    assert(rebuild()==-EAGAIN);
    model_remount();
    assert(!rebuild());
    model_remount();
    IdentityBinding current;
    PkiStorage storage(boot,settings,ctx,entropy,nullptr);
    assert(!storage.prepare()&&storage.preparedBinding(current)&&!reset::freshBinding(settings,current));
    assert(!reset::ready(settings,head)&&!reset::Floor::activate(flash,head,current.publicKey));
    model_remount();
    assert(!reset::inspect(flash,settings,head)&&head.state==reset::Floor::State::Active);
    assert(!memcmp(head.newKey,current.publicKey,32)&&memcmp(current.publicKey,original.publicKey,32));
    assert(!settings.eraseVerified(reset::RequestKey));
    uint8_t value=0;
    for(const char *key:owned)assert(settings.read(key,&value,1)==-ENOENT);
    assert(settings.read("bt-other/alpha",&value,1)==1&&settings.read("mtble/gatt-old/01",&value,1)==1);
    model_restore();model_remount();
    assert(!reset::inspect(flash,settings,head)&&reset::ready(settings,head)==-ENOENT);
    puts("PASS pinned four-sector NVS factory reset, real bond name selection and readback, archive removal, fresh boot and settings-only rollback refusal");
}
