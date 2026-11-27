// SPDX-License-Identifier: GPL-3.0-only
#include "modules/BeaconController.h"
#include "known_channel_fixture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

// Functions in donor.inc are unmodified, hash-pinned stock source bodies.
namespace donor {
using ChannelIndex=unsigned;
struct Channels {
    struct Key{const uint8_t *bytes;int length;};
    Key getKey(ChannelIndex){return {bench::MtNodeRecord::PublicKey,16};}
    meshtastic_Channel ch{};
    auto &getByIndex(ChannelIndex){return ch;}
    int16_t hashFor(ChannelIndex,const char *);
};
inline unsigned pow_of_2(unsigned n){return 1u<<n;}
inline long map(long v,long a,long b,long c,long d){return (v-a)*(d-c)/(b-a)+c;}
inline unsigned draw;
inline long random(long lo,long hi){return lo+draw%unsigned(hi-lo);}
struct Air {float utilization=0;float channelUtilizationPercent()const{return utilization;}} air;
static auto *airTime=&air;
struct Region{bool wideLora=false;} region;
static auto *myRegion=&region;
using std::max;
struct RadioInterface {
    static constexpr unsigned CWmin=3,CWmax=8;
    unsigned sf=0,slotTimeMsec=0;float bw=0;
    int getCadSymbolCount()const{return 4;}
    uint32_t computeSlotTimeMsec();
    uint32_t getTxDelayMsec();
};
struct MeshBeaconModule_TargetRadioSettings { int channelHash=-1; };
struct MeshBeaconModule {
    inline static bool switchNeeded=false,invalid=false;
    inline static MeshBeaconModule_TargetRadioSettings target{};
    static bool beaconTxConfigInvalid(const meshtastic_MeshPacket *){return invalid;}
    static const auto *getTargetRadioSettings(const meshtastic_MeshPacket *){return &target;}
    static bool reconfigureForBeaconTX(RadioInterface *,meshtastic_MeshPacket *){
        const bool toggle=switchNeeded;switchNeeded=false;return toggle;
    }
};
struct RadioTxHook {enum PreTxAction {PRETX_SEND,PRETX_DEFER,PRETX_DROP};};
struct MeshBeaconTxHook : RadioTxHook {
    PreTxAction beforeTransmit(RadioInterface *,meshtastic_MeshPacket *);
};
#define LOG_DEBUG(...) ((void)0)
#include "donor.inc"
#undef LOG_DEBUG
}
using Runtime=mt::modules::BeaconTargetRuntime;
using Excursion=mt::modules::BeaconExcursion;
using Bytes=std::vector<uint8_t>;
constexpr uint32_t Self=0x12345678u;
constexpr auto Medium=meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST;
static unsigned draw=5,draws=0;
static uint32_t nextDraw(){++draws;return draw;}
struct TestRadio final : mt::Radio {
    mt::Config applied;
    bool isolated=false,blockScan=false,complete=false,rxBusy=false;
    int state=0;
    unsigned scans=0,configures=0;
    std::vector<Bytes> frames;
    bool ready()override{return true;}
    bool busy()override{return rxBusy;}
    int start()override{state=1;return 0;}
    int stop()override{state=0;return 0;}
    int restore()override{state=1;return 0;}
    int health()override{return state;}
    bool beaconIsolationSupported()const override{return true;}
    void beaconRxIsolation(bool b)override{isolated=b;}
    int configure(const mt::Config &c)override{assert(state==0);applied=c;++configures;return 0;}
    uint32_t airtime(size_t)override{return 100;}
    int send(uint8_t *bytes,size_t n)override{
        assert(state==1);++scans;
        if(blockScan)return -EBUSY;
        state=2;frames.emplace_back(bytes,bytes+n);return 0;
    }
    bool completion(int &r)override{if(!complete)return false;complete=false;r=0;return true;}
};
struct Fixture {
    TestRadio radio;
    mt::test::KnownChannelFixtureClock clock;
    mt::Service service{radio,true,Self,127,true,false};
    Runtime runtime{service,nextDraw};
    mt::Config home;Bytes plain;
    Fixture(){
        clock.bind(service);
        auto request=home.radioRequest();request.regionCode=1;request.usePreset=true;request.preset=0;
        request.channel=60;request.overrideMHz=915;request.powerSpecified=true;request.power=30;
        home.setTxEnabled(true);assert(home.setRadio(request)&&home.primaryName()[0]==0);
        assert(service.apply(home)&&service.start(false,0)&&service.armEndpoint(true));
        meshtastic_ModuleConfig_MeshBeaconConfig cfg{};cfg.has_broadcast_offer_preset=true;
        cfg.broadcast_offer_preset=Medium;strcpy(cfg.broadcast_message,"mesh beacon");
        mt::stock::BeaconPayloadBatch batch{};
        assert(mt::stock::MeshBeaconBroadcastModule::sendBeacon(cfg,batch,1)&&batch.count==1);
        batch.packets[0].has_bitfield=true;
        plain.resize(239);auto out=pb_ostream_from_buffer(plain.data(),plain.size());
        assert(pb_encode(&out,meshtastic_Data_fields,&batch.packets[0]));plain.resize(out.bytes_written);
    }
    Excursion::ResolvedTarget resolve(unsigned preset,unsigned slot=0){
        meshtastic_ModuleConfig_MeshBeaconConfig cfg{};cfg.broadcast_targets_count=1;
        auto &t=cfg.broadcast_targets[0];t.has_preset=true;t.preset=decltype(t.preset)(preset);
        t.has_frequency_slot=slot!=0;t.frequency_slot=slot;
        Excursion::ResolvedTarget targets[4]{};size_t n=0;
        assert(mt::modules::BeaconBroadcaster::resolveTargets(home,cfg,targets,n)&&n==1);
        return targets[0];
    }
    bool enqueue(uint32_t id,const Excursion::ResolvedTarget &t,uint32_t now,bool legacy=false){
        Excursion::Request req{};req.packetId=id;req.identityEpoch=Self;req.settingsEpoch=1;
        req.target=t.profile;req.requiresRetune=t.requiresRetune;req.legacyHopOverride=legacy;
        return runtime.enqueue(req,plain.data(),plain.size(),id,now);
    }
    void poll(uint32_t now,unsigned epoch=1){assert(clock.set(now));runtime.poll(now,epoch,Self,true);}
    void finish(uint32_t now,unsigned epoch=1){radio.complete=true;poll(now,epoch);}
    unsigned delay(unsigned choice){
        mt::LoRaPresets::Profile profile{};mt::USChannelSelection channel{};
        assert(radio.applied.resolveRadio(profile,channel));
        donor::RadioInterface iface;iface.sf=profile.spreadingFactor;iface.bw=profile.bandwidthHz/1000.0f;
        iface.slotTimeMsec=iface.computeSlotTimeMsec();donor::draw=choice;
        donor::air.utilization=*service.channelMeter().percent();
        return iface.getTxDelayMsec();
    }
    void frame(unsigned index,uint32_t id,const char *name,bool legacy=false){
        const auto &got=radio.frames.at(index);
        donor::Channels channels;const int hash=channels.hashFor(0,name);assert(hash>=0);
        bench::MtCodec codec;assert(codec.setKey(uint8_t(hash),bench::MtNodeRecord::PublicKey,16));
        size_t n=0;uint8_t expected[255]{};
        assert(codec.encodeZeroHopData(Self,UINT32_MAX,id,plain.data(),plain.size(),expected,sizeof(expected),n));
        expected[12]=legacy?0x20:0;
        assert(got.size()==n&&!memcmp(expected,got.data(),n));
        bench::RouteMetadata metadata{};assert(codec.decode(got.data(),got.size(),metadata));
        assert(metadata.portnum==meshtastic_PortNum_MESH_BEACON_APP);
    }
};
static void offChannel(){
    Fixture f;const auto target=f.resolve(Medium);
    assert(target.requiresRetune);
    assert(f.enqueue(100,target,0,true));
    donor::Channels channels;
    donor::MeshBeaconModule::target.channelHash=channels.hashFor(0,"MediumFast");
    donor::MeshBeaconModule::switchNeeded=true;
    donor::MeshBeaconTxHook hook;meshtastic_MeshPacket packet{};packet.id=100;
    assert(hook.beforeTransmit(nullptr,&packet)==donor::RadioTxHook::PRETX_DEFER);
    draw=5;draws=0;f.poll(100);
    assert(f.radio.isolated&&f.radio.scans==0&&draws==1);
    assert(f.radio.applied.radioRequest().overrideMHz==0&&f.radio.applied.modemPreset()==Medium);
    uint32_t at=100+f.delay(draw);assert(at>100);f.poll(at-1);assert(f.radio.scans==0);
    f.radio.blockScan=true;draw=2;f.poll(at);
    assert(f.radio.scans==1&&f.radio.frames.empty()&&f.radio.isolated&&draws==2);
    at+=f.delay(draw);f.radio.blockScan=false;f.poll(at-1);assert(f.radio.scans==1);
    assert(hook.beforeTransmit(nullptr,&packet)==donor::RadioTxHook::PRETX_SEND);
    f.poll(at);assert(f.radio.scans==2&&f.radio.frames.size()==1&&f.radio.frames[0][13]==packet.channel);
    f.frame(0,100,"MediumFast",true);
    assert(f.radio.frames[0][13]!=f.service.codec.channelHash());
    f.finish(at+1);assert(!f.radio.isolated&&!f.runtime.busy());
    assert(f.radio.applied.radioRequest().overrideMHz==915&&f.radio.applied.radioRequest().power==30);
    assert(f.radio.applied.channelNumber()==60&&f.service.receiving()&&f.service.endpointArmed());
}
static void reusedSlots(){
    Fixture f;Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    for(unsigned i=1;i<=3;++i)assert(f.enqueue(i,home,0));
    f.poll(10);f.frame(0,1,"LongFast");f.finish(11);
    assert(f.enqueue(4,home,12)); // physical slot zero is now below the older requests
    for(unsigned id=2;id<=4;++id){f.poll(id*10);f.frame(id-1,id,"LongFast");f.finish(id*10+1);}
    assert(!f.runtime.busy()&&f.radio.scans==4&&!f.radio.isolated);
}
static void changedHome(){
    Fixture f;
    // Home pins slot 60; the stock LONG_TURBO target has fewer slots. An
    // unspecified target slot must use the target's own geometry.
    auto target=f.resolve(meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO);
    assert(target.profile.slot>0&&target.profile.slot<60&&f.enqueue(7,target,0));
    draw=3;f.poll(5);assert(f.radio.isolated&&f.radio.scans==0);
    auto edit=f.service.config.radioRequest();edit.overrideMHz=916;
    assert(f.service.config.setRadio(edit));
    f.poll(5+f.delay(draw),2);
    assert(!f.radio.isolated&&f.radio.frames.empty()&&f.radio.applied.radioRequest().overrideMHz==916);
    donor::MeshBeaconModule::invalid=true;
    donor::MeshBeaconTxHook hook;meshtastic_MeshPacket packet{};
    assert(hook.beforeTransmit(nullptr,&packet)==donor::RadioTxHook::PRETX_DROP);
    auto invalid=target;invalid.profile.slot=999999;assert(!f.enqueue(9,invalid,1000));
}
int main(){offChannel();reusedSlots();changedHome();std::puts("{\"target_rechecks\":2,\"fifo_frames\":4,\"runtime_scenarios\":3}");}
