// SPDX-License-Identifier: GPL-3.0-only
// Actual NVS read work during USB-local identity enrollment. Synthetic NOR only.
#include "pki/NvsProvisioningSettings.h"
#include "bridge.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace mt::pki;
struct Context : ProvisioningContext, PkiStorageContext {
    unsigned depth = 0;
    bool tryEnter() override { ++depth; return true; }
    void leave() override { assert(depth); --depth; }
    bool quiescent() const override { return depth != 0; }
    bool bootQuiescent() const override { return quiescent(); }
    uint64_t millis() const override { return 100; }
    bool candidateAllowed(const IdentityBinding &) const override { return true; }
    bool peerAllowed(const IdentityBinding &, const ProvisionPeer &) const override { return true; }
};
struct Entropy : EntropySource {
    bool fill(uint8_t *out, size_t n) override {
        for (size_t i = 0; i < n; ++i) out[i] = uint8_t(21 + 23 * i);
        return true;
    }
};
struct ScanMeter {
    unsigned last=0, lastFlash=0, count=0, worst=0, worstFlash=0;
    unsigned captureAfterWrites=0;
    bool capturePending=false, captured=false;
    void restart(){last=model_reads();lastFlash=model_flash_reads();count=worst=worstFlash=0;}
    void advance(){
        const unsigned now=model_reads();
        assert(now>last);
        if(now-last>worst)worst=now-last;
        const unsigned flash=model_flash_reads();
        assert(flash>lastFlash);
        if(flash-lastFlash>worstFlash)worstFlash=flash-lastFlash;
        last=now;lastFlash=flash;
        ++count;
        if(capturePending && model_writes()>captureAfterWrites){
            model_snapshot(); // First checked reads after the pending intent was persisted.
            capturePending=false;captured=true;
        }
    }
    static void observe(void *context){
        static_cast<ScanMeter *>(context)->advance();
    }
};

static void watchdogProgress(unsigned sectors)
{
    static uint8_t baseline[32768], observed[sizeof(baseline)];
    for(unsigned enabled=0;enabled<2;++enabled){
        model_reset(sectors);
        const uint8_t unrelated[]={1,2,3,4};
        for(unsigned i=0;i<24;++i){
            char name[32];snprintf(name,sizeof(name),"other/fixture-%u",i);
            assert(settings_save_one(name,unrelated,sizeof(unrelated))==0);
        }
        Context ctx; Entropy entropy; ScanMeter meter;
        NvsProvisioningSettings setup;
        if(enabled)setup.setScanProgress(ScanMeter::observe,&meter);
        PkiFreshProvisioning core(setup,ctx,entropy);
        IdentityBinding candidate;
        meter.restart();
        const unsigned beforeStage=model_reads();
        assert(core.stage(candidate)==0);
        const unsigned stageReads=model_reads()-beforeStage;
        const unsigned stageCheckpoints=meter.count;
        meter.restart();
        const unsigned beforeConfirm=model_reads(), beforeFlash=model_flash_reads();
        meter.captureAfterWrites=model_writes();
        meter.capturePending=enabled!=0;
        assert(core.confirm(candidate)==0);
        if(enabled){
            const unsigned confirmReads=model_reads()-beforeConfirm;
            assert(stageCheckpoints>=8 && stageCheckpoints%8==0);
            assert(meter.count>stageCheckpoints && meter.count%8==0);
            const unsigned flashReads=model_flash_reads()-beforeFlash;
            assert(meter.worst<=96 && model_reads()-meter.last<=8);
            assert(meter.worstFlash<flashReads/10);
            model_copy_flash(observed);
            assert(!memcmp(baseline,observed,sizeof(baseline)));
            assert(meter.captured);
            printf("PASS %u sectors, stage %u NVS reads/%u scans; confirm %u reads/%u scans, "
                   "%u underlying NOR reads, max %u NVS/%u NOR reads between callbacks, "
                   "identical full NOR bytes\n",sectors,
                   stageReads,stageCheckpoints/8,confirmReads,meter.count/8,flashReads,meter.worst,meter.worstFlash);
            model_restore(); // New boot with durable intent, before the first nonce write.
            struct NoEntropy:EntropySource {
                bool fill(uint8_t *,size_t)override{assert(false);return false;}
            } noEntropy;
            ScanMeter resumedMeter;
            NvsProvisioningSettings resumedSettings;
            resumedSettings.setScanProgress(ScanMeter::observe,&resumedMeter);
            PkiFreshProvisioning resumed(resumedSettings,ctx,noEntropy);
            IdentityBinding same;
            resumedMeter.restart();
            const unsigned persistedWrites=model_writes();
            assert(resumed.stage(same)==0 && resumedMeter.count>0 && model_writes()==persistedWrites);
            assert(same.node==candidate.node && !memcmp(candidate.publicKey,same.publicKey,32));
            assert(resumed.confirm(same)==0);
            NvsBootSettings boot;mt::ZephyrSettings runtime;
            PkiStorage storage(boot,runtime,ctx,noEntropy,nullptr);
            assert(storage.prepare()==0 && storage.activate(same.node)==0);
            IdentityBinding stored;KeyRef ref;
            assert(storage.runtimeIdentity()->binding(stored,ref) && stored.node==same.node &&
                   !memcmp(stored.publicKey,same.publicKey,32));
        }else{
            assert(!meter.count);
            model_copy_flash(baseline);
        }
    }
    // Taking a busy lock, an invalid high-water mark, and a failed row cannot
    // manufacture progress. Exactly one 16-row group precedes the fault at row 17.
    ScanMeter meter;
    NvsProvisioningSettings setup;setup.setScanProgress(ScanMeter::observe,&meter);
    meter.restart();model_busy(1);
    assert(setup.begin()==-EBUSY && meter.count==0);
    model_busy(0);
    model_read_error(0x8011,-EIO);
    assert(setup.begin()==-EIO && meter.count==1);
    model_no_fault();model_counter(0x8081);
    assert(setup.begin()==-EILSEQ && meter.count==1);
}

int main()
{
    watchdogProgress(8);
    watchdogProgress(4);
    puts("PASS cold resume of exact pending identities and checked progress on both geometries");
}
