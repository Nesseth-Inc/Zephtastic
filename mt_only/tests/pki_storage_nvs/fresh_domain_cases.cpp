// SPDX-License-Identifier: GPL-3.0-only
// Production setup/boot/nonce/contacts on mounted 4 x 4096-byte NVS and synthetic NOR.
#include "pki/NvsProvisioningSettings.h"
#include "pki/PkiFreshUsbIngress.h"
#include "bridge.h"
#include "monocypher.h"
#include <cassert>
#include <initializer_list>
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
struct Session {
    bool bind(uint32_t epoch) { return epoch == 1; }
    bool current(uint32_t epoch) const { return epoch == 1; }
    bool current() const { return true; }
    void clear() {}
};

static unsigned withdrawals=0;
static int recordWithdrawal(uint32_t){++withdrawals;return 0;}
static int failedWithdrawal(uint32_t){return -EIO;}

static PkiPeerPin makePeer(unsigned id)
{
    PkiPeerPin p{};
    uint8_t scalar[32];
    for (unsigned i = 0; i < 32; ++i) scalar[i] = uint8_t(11 * id + i);
    crypto_x25519_public_key(p.publicKey, scalar);
    p.node = bench::MtSignature::nodeId(p.publicKey);
    return p;
}

static IdentityBinding provision(NvsProvisioningSettings &setup, Context &ctx, Entropy &entropy)
{
    Session session;
    PkiFreshProvisioning core(setup, ctx, entropy);
    PkiFreshUsbIngress<Session> usb(core, session);
    const char stage[] = "zt_pki_fresh_v1 stage";
    char response[256]{};
    const auto writes = model_writes();
    assert(usb.execute(reinterpret_cast<const uint8_t *>(stage), strlen(stage), 1, response, sizeof(response)) > 0);
    assert(strstr(response, "rc=0 ") && writes == model_writes());
    IdentityBinding candidate;
    assert(core.stage(candidate) == 0);
    char command[128];
    int n = snprintf(command, sizeof(command), "zt_pki_fresh_v1 confirm %08x ", candidate.node);
    for (uint8_t b : candidate.publicKey) n += snprintf(command+n, sizeof(command)-n, "%02x", b);
    assert(usb.execute(reinterpret_cast<const uint8_t *>(command), size_t(n), 1, response, sizeof(response)) > 0);
    assert(strstr(response, "rc=0 ") && strstr(response, "reboot=1") && model_writes() > writes);
    return candidate;
}

// Inject a single boot read through the real storage adapter. Its exact-size
// contracts apply to orphan/corrupt rows as they do to ordinary NVS records.
struct BootRow : BootSettings {
    BootSettings &base;
    const char *name;
    int result;
    size_t required;
    unsigned hits=0;
    BootRow(BootSettings &b,const char *n,int v,size_t size):base(b),name(n),result(v),required(size){}
    int begin() override{return base.begin();}
    void end() override{base.end();}
    int read(const char *key,uint8_t *dst,size_t cap) override{
        if(strcmp(key,name))return base.read(key,dst,cap);
        assert(cap==required);++hits;
        if(result>=0)memset(dst,0x5a,size_t(result));
        return result;
    }
};
static void orphanRows(NvsBootSettings &boot,mt::ZephyrSettings &runtime,Context &ctx,Entropy &entropy,bool selected){
    const char *keys[]={"mtpki/ledger0","mtpki/ledger1","mtpki/nonces0","mtpki/nonces1","mtpki/contacts0","mtpki/contacts1"};
    for(unsigned n=0;n<(selected?2u:6u);++n){
        const size_t size=n<2?PkiLedger::RecordSize:n<4?PkiNonceCounter::RecordBytes:PkiContactSnapshot::RecordBytes;
        for(int error:{int(size),1,-EIO}){
            BootRow row(boot,keys[n],error,size);PkiStorage storage(row,runtime,ctx,entropy,nullptr);
            const auto writes=model_writes();
            assert(storage.prepare()==(error<0?error:error==1?-EILSEQ:-EEXIST));
            assert(row.hits==1&&!storage.runtimeIdentity()&&model_writes()==writes);
            const auto state=error<0?PkiStorage::State::Frozen:error==1?PkiStorage::State::Corrupt:PkiStorage::State::Partial;
            assert(storage.state()==state);
        }
    }
}
int main()
{
    model_reset(4);
    model_inventory("Empty 4-sector partition");
    NvsBootSettings boot;
    mt::ZephyrSettings settings;
    NvsProvisioningSettings setup;
    Context ctx;
    Entropy entropy;
    {
        PkiStorage empty(boot, settings, ctx, entropy, nullptr);
        assert(empty.prepare() == -ENOENT && empty.state() == PkiStorage::State::Empty);
    }
    orphanRows(boot,settings,ctx,entropy,false);
    const IdentityBinding id = provision(setup, ctx, entropy);
    orphanRows(boot,settings,ctx,entropy,true);
    model_remount();
    auto peer = makePeer(1);
    uint32_t first = 0, through = 0;
    {
        PkiStorage storage(boot, settings, ctx, entropy, nullptr);
        const auto writes = model_writes();
        assert(storage.prepare() == 0 && model_writes() == writes);
        assert(storage.activate(id.node) == 0 && storage.boundedHistory() && storage.compactContacts());
        IdentityBinding read; KeyRef ref;
        assert(storage.runtimeIdentity()->binding(read, ref) && read.node == id.node);
        assert(storage.putOwnerPeer(id, peer) == 0 && storage.active(peer.node, peer.publicKey));
        assert(storage.reserve(ref,id.publicKey,id.node,51,first));
        through = storage.nonceReservedThrough();
        assert(through >= first);
        assert(storage.commitAuthenticated(peer.node,peer.publicKey,91));
        assert(!storage.commitAuthenticated(peer.node,peer.publicKey,91));
        auto second=makePeer(2);
        const auto before=model_writes();
        assert(storage.putOwnerPeer(id,second,failedWithdrawal)==-EIO&&model_writes()==before&&
               !storage.active(second.node,second.publicKey));
        for (unsigned i = 2; i <= 16; ++i) assert(storage.putOwnerPeer(id,makePeer(i),recordWithdrawal) == 0);
        const auto full=model_writes();
        assert(storage.putOwnerPeer(id,makePeer(17),recordWithdrawal)==-ENOSPC&&withdrawals==15&&model_writes()==full);
        auto duplicate=peer;duplicate.node=second.node;
        assert(storage.putOwnerPeer(id,duplicate,recordWithdrawal)==-EEXIST&&withdrawals==15&&model_writes()==full);
    }
    model_remount();
    {
        PkiStorage storage(boot, settings, ctx, entropy, nullptr);
        assert(storage.prepare() == 0 && storage.activate(id.node) == 0);
        IdentityBinding read; KeyRef ref;
        assert(storage.runtimeIdentity()->binding(read,ref) && read.node == id.node);
        uint32_t next = 0; assert(storage.reserve(ref,id.publicKey,id.node,51,next) && next > through);
        assert(storage.active(peer.node,peer.publicKey));
        model_inventory("Fresh 16-peer domain with both nonce checkpoints");
        assert(storage.commitAuthenticated(peer.node,peer.publicKey,91)); // bounded RX restarts empty
        assert(storage.revokeTrustedPeer(peer.node,peer.publicKey));
    }
    // Each reboot forfeits the remainder of its reserved TX block. Repeated
    // boots force the real mounted NVS backend through several garbage collections.
    const unsigned eraseBefore = model_erases();
    for (unsigned cycle = 0; cycle < 384; ++cycle) {
        model_remount();
        PkiStorage storage(boot, settings, ctx, entropy, nullptr);
        assert(storage.prepare() == 0 && storage.activate(id.node) == 0);
        IdentityBinding local; KeyRef ref;
        assert(storage.runtimeIdentity()->binding(local,ref) && local.node == id.node);
        for (unsigned packet = 0; packet < 4; ++packet) {
            uint32_t nonce = 0;
            assert(storage.reserve(ref,id.publicKey,id.node,51 + packet,nonce) && nonce > through);
            through = nonce;
        }
        through = storage.nonceReservedThrough();
        assert(!storage.active(peer.node,peer.publicKey));
        auto retained = makePeer(16);
        assert(storage.active(retained.node,retained.publicKey));
    }
    assert(model_erases() > eraseBefore);
    printf("PASS 384 reboots with 1,536 TX reservations and %u modeled page erases; final reserved-through=%u\n",
           model_erases()-eraseBefore,through);
    model_remount();
    {
        PkiStorage storage(boot, settings, ctx, entropy, nullptr);
        assert(storage.prepare() == 0 && storage.activate(id.node) == 0);
        assert(!storage.active(peer.node,peer.publicKey));
        auto other = makePeer(2); assert(storage.active(other.node,other.publicKey));
    }
    // Exercise the same non-PKI settings sizes as the repository's BT/NVS capacity fixture.
    // This models competition for flash, without manufacturing valid application or pairing state.
    struct OtherSetting {const char *key; size_t bytes;};
    const OtherSetting nonPki[]={{"mtbase/config",80},{"mtbase/state0",624},{"mtbase/state1",624},
        {"mtbase/nodes0",1492},{"mtbase/nodes1",1492},{"mtmod/state0",128},{"mtmod/state1",128},
        {"mtble/state0",32},{"mtble/state1",32},{"bt/id",7},{"bt/irk",16},{"bt/name",18},{"bt/hash",16}};
    uint8_t sample[1492],loaded[1492];
    for(unsigned i=0;i<sizeof(sample);++i)sample[i]=uint8_t(i*13+7);
    auto otherKeys=[&](auto check){
        for(const auto &item:nonPki)check(item.key,item.bytes);
        for(unsigned n=1;n<=5;++n){
            for(const auto &item:{OtherSetting{"keys",56},OtherSetting{"ccc",8},OtherSetting{"sc",4},OtherSetting{"cf",2}}){
                char key[40];snprintf(key,sizeof(key),"bt/%s/%012u0",item.key,n);
                check(key,item.bytes);
            }
        }
    };
    otherKeys([&](const char *key,size_t n){assert(settings_save_one(key,sample,n)==0);});
    model_remount();
    otherKeys([&](const char *key,size_t n){assert(settings_load_one(key,loaded,sizeof(loaded))==ssize_t(n)&&!memcmp(sample,loaded,n));});
    model_inventory("Mixed 16-peer domain plus base/module settings and 5 BLE bonds");
    // Satellite packet data lives on a second volume; its only extra NVS costs
    // are a resettable enrollment epoch and the per-peer lifetimes in NodeJournal v7.
    {
        uint8_t v7[1572]{};memset(v7,87,sizeof(v7));uint8_t check[sizeof(v7)]{};
        assert(settings_save_one("mtbase/nodes0",v7,sizeof(v7))==0);
        assert(settings_save_one("mtbase/nodes1",v7,sizeof(v7))==0);
        assert(settings_save_one("mtsat/epoch",v7,16)==0);model_remount();
        for(auto key:{"mtbase/nodes0","mtbase/nodes1"}){
            assert(settings_load_one(key,check,sizeof(check))==sizeof(v7)&&!memcmp(check,v7,sizeof(v7)));
        }
        assert(settings_load_one("mtsat/epoch",check,sizeof(check))==16&&!memcmp(check,v7,16));
        model_inventory("Mixed domain with two satellite-enabled v7 NodeJournals and history epoch");
    }
    {
        PkiStorage storage(boot,settings,ctx,entropy,nullptr);
        assert(storage.prepare()==0 && storage.activate(id.node)==0);
        auto last=makePeer(16);assert(storage.active(last.node,last.publicKey)&&!storage.active(peer.node,peer.publicKey));
    }
    uint8_t isolatedIdentity[IdentityJournal::RecordSize];
    assert(settings_load_one("mtidentity/state0",isolatedIdentity,sizeof(isolatedIdentity))==sizeof(isolatedIdentity));
    // A malformed selector on an enrolled domain prevents opening and reseeding.
    mt::ZephyrSettings writer;
    uint8_t legacy[8] = {'o','l','d'}; // malformed root must also hold the domain
    assert(!writer.write(PkiFreshProvisioning::RootKey,legacy,sizeof(legacy)));
    model_remount();
    PkiStorage broken(boot, settings, ctx, entropy, nullptr);
    const auto writes = model_writes();
    assert(broken.prepare() < 0 && !broken.runtimeIdentity() && model_writes() == writes);
    PkiFreshProvisioning repeat(setup,ctx,entropy); IdentityBinding replacement;
    assert(repeat.stage(replacement) < 0 && model_writes() == writes);
    // Reuse the exact valid identity record, with every selector, intent, nonce,
    // contact and ledger record absent. This exercises the separate legacy guard.
    model_reset(4);
    assert(settings_save_one("mtidentity/state0",isolatedIdentity,sizeof(isolatedIdentity))==0);
    crypto_wipe(isolatedIdentity,sizeof(isolatedIdentity));
    model_remount();
    const auto legacyWrites=model_writes();
    PkiStorage identityOnly(boot,settings,ctx,entropy,nullptr);
    assert(identityOnly.prepare()==-ENOTSUP && !identityOnly.runtimeIdentity() && model_writes()==legacyWrites);
    PkiFreshProvisioning legacySetup(setup,ctx,entropy);
    assert(legacySetup.stage(replacement)<0 && model_writes()==legacyWrites);
    puts("PASS fresh USB genesis, NVS-only boot, 16 contacts, reboot/revoke, nonce reservation and replay boundaries, refused reseed");
}
