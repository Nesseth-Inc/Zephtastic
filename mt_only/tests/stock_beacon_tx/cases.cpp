// SPDX-License-Identifier: GPL-3.0-only
#include "modules/BeaconController.h"
#include "known_channel_fixture.h"
#include "relay_fixture_provider.h"
#include <map>
#include <string>
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
// Use the production drain, including its native support decision while the
// application receiver is isolated. Driver calls below model an empty queue.
namespace meterOwner {
struct ApplicationReceiver {
    const mt::Service *source=nullptr;
    bool receiving()const{return source&&source->receiving();}
} service;
static unsigned beaconOffProfileRx=0,radioDevice=0;
static bool nativeSupport=true;
static unsigned atomic_get(const unsigned *v){return *v;}
struct sx126x_airtime_observation {uint32_t airtime_ms=0;bool known=false;uint64_t observed_at_ms=0;uint16_t irq=0;uint8_t length=0;};
static bool sx126x_airtime_observer_supported(unsigned){return nativeSupport;}
static bool sx126x_airtime_observer_take_loss(unsigned){return false;}
static int sx126x_airtime_observer_take(unsigned,sx126x_airtime_observation *){return -ENOENT;}
#include "meter-owner.inc"
}
struct TestRadio final : mt::Radio {
    mt::Config applied;
    bool isolated=false,blockScan=false,complete=false,rxBusy=false,rxPending=false,rxCallback=false,retiredBusy=false;
    int state=0;
    unsigned scans=0,configures=0,homeFailures=0,startFailures=0,restoreFailures=0;
    unsigned aborts=0,invalidPauses=0,abortFailures=0,homeStartFailures=0,targetConfigureFailures=0;
    const uint8_t *borrowed=nullptr;size_t borrowedSize=0;
    std::vector<Bytes> frames;
    bool ready()override{return true;}
    bool busy()override{return rxBusy;}
    bool receiveEventPending()const override{return rxPending;}
    int start()override{
        // Native start applies RF parameters and arms the callback. An error
        // can leave an unverified partial configuration, also reporting health zero.
        auto &fail=applied.radioRequest().overrideMHz!=0?homeStartFailures:startFailures;
        if(fail){--fail;state=3;rxCallback=false;return -EIO;}
        state=1;rxCallback=true;return 0;
    }
    int stop()override{
        assert(!borrowed);++aborts;
        if(abortFailures){--abortFailures;return -EIO;}
        state=0;rxCallback=false;return 0;
    }
    int stopForStorage()override{
        if(rxPending)return -EAGAIN;
        if(state!=1||!rxCallback){++invalidPauses;return -EIO;}
        // A sticky IRQ after native standby/retirement is a hard raw -EBUSY.
        if(retiredBusy){retiredBusy=false;state=3;rxCallback=false;return -EBUSY;}
        state=0;rxCallback=false;return 0;
    }
    int restore()override{if(restoreFailures){--restoreFailures;return -EIO;}state=1;rxCallback=true;return 0;}
    int health()override{return state==1?1:0;}
    bool beaconIsolationSupported()const override{return true;}
    void beaconRxIsolation(bool b)override{isolated=b;meterOwner::beaconOffProfileRx=b;}
    int configure(const mt::Config &c)override{
        assert(state==0);++configures;
        auto &fail=c.radioRequest().overrideMHz!=0?homeFailures:targetConfigureFailures;
        if(fail){--fail;return -EIO;}
        applied=c;return 0;
    }
    uint32_t airtime(size_t)override{return 100;}
    int send(uint8_t *bytes,size_t n)override{
        assert(state==1);++scans;
        if(blockScan)return -EBUSY;
        state=2;borrowed=bytes;borrowedSize=n;frames.emplace_back(bytes,bytes+n);return 0;
    }
    bool completion(int &r)override{
        if(!complete)return false;
        assert(borrowed&&frames.back().size()==borrowedSize&&!memcmp(borrowed,frames.back().data(),borrowedSize));
        borrowed=nullptr;borrowedSize=0;complete=false;r=0;return true;
    }
};
struct Fixture {
    TestRadio radio;
    mt::test::KnownChannelFixtureClock clock;
    mt::Service service;
    Runtime runtime{service,nextDraw};
    mt::Config home;Bytes plain;
    explicit Fixture(bool relay=false,mt::pki::PrivateRuntime *provider=nullptr,void(*beforeStart)(Fixture &)=nullptr):
        service(radio,true,Self,127,true,false,provider){
        meterOwner::service.source=&service;meterOwner::beaconOffProfileRx=0;meterOwner::nativeSupport=true;
        const auto clockNow=[](void *ctx){return static_cast<mt::test::KnownChannelFixtureClock *>(ctx)->now();};
        assert(service.channelMeter().bind({clockNow,&clock},meterOwner::drainChannelObservations));
        auto request=home.radioRequest();request.regionCode=1;request.usePreset=true;request.preset=0;
        request.channel=60;request.overrideMHz=915;request.powerSpecified=true;request.power=30;
        if(relay)assert(home.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT,mt::DeviceRolePolicy::RebroadcastMode::ALL));
        home.setTxEnabled(true);assert(home.setRadio(request)&&home.primaryName()[0]==0);
        if(beforeStart)beforeStart(*this);
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
    bool enqueue(uint32_t id,const Excursion::ResolvedTarget &t,uint32_t now,bool legacy=false,uint32_t group=0){
        Excursion::Request req{};req.packetId=id;req.identityEpoch=Self;req.settingsEpoch=1;
        req.groupId=group;req.target=t.profile;req.requiresRetune=t.requiresRetune;req.legacyHopOverride=legacy;
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
static void refill(Fixture &f,uint32_t now){
    Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    for(unsigned i=0;i<Excursion::QueueCapacity;++i)assert(f.enqueue(1000+i,home,now));
    assert(!f.enqueue(2000,home,now)&&f.runtime.queued()==Excursion::QueueCapacity);
    for(unsigned i=0;i<Excursion::QueueCapacity;++i){
        const auto frame=f.radio.frames.size();f.poll(++now);f.frame(frame,1000+i,"LongFast");f.finish(++now);
    }
    assert(!f.runtime.busy());
}
static void homeRestoreRetry(){
    Fixture f;const auto target=f.resolve(Medium);draw=1;
    assert(f.enqueue(50,target,0));f.poll(10);
    auto now=10+f.delay(draw);f.poll(now);f.frame(0,50,"MediumFast");
    f.radio.homeFailures=3;f.radio.homeStartFailures=1;f.finish(++now);
    for(unsigned retry=0;retry<2;++retry){
        assert(f.runtime.radioHeld()&&f.radio.isolated&&!f.service.receiving());
        f.poll(++now);
    }
    f.poll(++now);
    assert(f.runtime.radioHeld()&&f.radio.isolated&&f.radio.state==3&&f.radio.health()==0);
    f.radio.abortFailures=1;f.poll(++now);
    assert(f.runtime.radioHeld()&&f.radio.isolated&&f.radio.state==3);
    f.poll(++now);assert(!f.runtime.busy()&&!f.radio.isolated&&f.service.receiving());
    assert(f.radio.aborts==2&&!f.radio.invalidPauses);
    // A later request gets its own admission timeout after the restore retry.
    now+=6000;assert(f.enqueue(51,target,now));f.poll(now);now+=f.delay(draw);
    f.radio.blockScan=true;f.poll(now);f.radio.blockScan=false;
    now+=f.delay(draw);f.poll(now);f.frame(1,51,"MediumFast");f.finish(++now);
    refill(f,++now);
}
static void delayedSplit(){
    Fixture f;const auto target=f.resolve(Medium);draw=1;
    assert(f.enqueue(60,target,0,false,3)&&f.enqueue(61,target,0,true,3));
    f.poll(10);auto now=10+f.delay(draw);f.poll(now);f.frame(0,60,"MediumFast");
    f.radio.rxBusy=true;f.finish(++now);
    assert(f.runtime.radioHeld()&&f.radio.frames.size()==1&&f.radio.isolated);
    f.radio.rxBusy=false;now+=f.delay(draw);f.poll(now);f.frame(1,61,"MediumFast",true);
    assert(f.radio.isolated);f.finish(++now);
    assert(!f.radio.isolated&&!f.runtime.busy());refill(f,++now);
}
static void cancelInFlight(){
    Fixture f;const auto target=f.resolve(Medium);draw=1;
    assert(f.enqueue(70,target,0)&&f.enqueue(71,target,0));
    f.poll(10);auto now=10+f.delay(draw);f.poll(now);f.frame(0,70,"MediumFast");
    f.runtime.cancelAll(1,Self);
    assert(f.runtime.queued()==0&&f.runtime.radioHeld()&&f.radio.isolated);
    assert(f.radio.borrowedSize==f.radio.frames[0].size()&&
           !memcmp(f.radio.borrowed,f.radio.frames[0].data(),f.radio.borrowedSize));
    f.finish(++now);assert(f.radio.frames.size()==1&&!f.radio.isolated&&!f.runtime.busy());
    refill(f,++now);
}
static void faultRecovery(){
    Fixture f;const auto target=f.resolve(Medium);draw=1;
    assert(f.enqueue(80,target,0,false,8)&&f.enqueue(81,target,0,true,8));
    f.poll(10);auto now=10+f.delay(draw);f.poll(now);f.frame(0,80,"MediumFast");
    f.radio.restoreFailures=1;f.finish(++now);
    assert(f.service.faulted()&&f.runtime.radioHeld()&&f.radio.isolated);
    f.poll(++now);assert(f.service.faulted()&&!f.runtime.radioHeld()&&!f.radio.isolated);
    assert(f.radio.frames.size()==1&&f.runtime.queued()==1);f.runtime.cancelAll(1,Self);
    assert(!f.runtime.busy()&&!f.enqueue(82,target,now));

    Fixture g;assert(g.enqueue(90,target,0));g.radio.startFailures=1;g.radio.homeFailures=2;
    g.poll(10);assert(g.radio.homeFailures==0&&g.runtime.radioHeld()&&g.service.faulted());
    // A closed owner gate still owes the device a verified return to home RX.
    g.runtime.poll(11,1,Self,false);
    assert(!g.runtime.busy()&&!g.radio.isolated&&g.radio.health()==1&&g.service.receiving());
    assert(g.service.faulted()&&g.radio.frames.empty()&&g.radio.applied.radioRequest().overrideMHz==915);
    assert(g.radio.aborts==1&&!g.radio.invalidPauses);

    Fixture h;assert(h.enqueue(91,target,0));h.radio.targetConfigureFailures=1;
    h.poll(10);assert(!h.runtime.busy()&&!h.radio.isolated&&h.radio.frames.empty());
    assert(!h.service.faulted()&&!h.radio.aborts&&!h.radio.invalidPauses&&h.service.receiving());
}
static void retiredTarget(){
    Fixture f;const auto target=f.resolve(Medium);draw=1;
    assert(f.enqueue(205,target,0));f.poll(10);auto now=10+f.delay(draw);f.poll(now);
    f.radio.retiredBusy=true;f.finish(++now);
    assert(f.service.faulted()&&f.runtime.radioHeld()&&f.radio.isolated&&!f.radio.rxCallback);
    f.poll(++now);
    assert(!f.runtime.radioHeld()&&f.service.faulted()&&!f.radio.isolated&&f.radio.rxCallback&&
           f.radio.aborts==1&&!f.radio.invalidPauses);
}
static void terminalSignal(){
    Fixture f;Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    assert(f.enqueue(210,home,0));f.poll(10);f.frame(0,210,"LongFast");
    f.radio.rxBusy=true;f.finish(11);
    assert(f.radio.busy()&&!f.runtime.radioHeld()&&f.service.receiving()&&!f.service.faulted());

    Fixture g;const auto target=g.resolve(Medium);draw=1;
    assert(g.enqueue(211,target,0));g.poll(10);auto at=10+g.delay(draw);g.poll(at);
    mt::RadioTiming::Deadline timing{};assert(mt::RadioTiming::resolve(g.radio.airtime(1),timing));
    at+=timing.hostMs;g.poll(at);
    assert(g.service.faulted()&&g.runtime.lastError()==-ETIMEDOUT&&g.runtime.radioHeld());
    g.runtime.cancelAll(1,Self);g.poll(++at);
    assert(g.radio.borrowed&&!memcmp(g.radio.borrowed,g.radio.frames[0].data(),g.radio.borrowedSize));
    g.finish(++at);assert(!g.runtime.radioHeld()&&!g.radio.isolated&&g.service.faulted()&&!g.radio.borrowed);
}
static void normalSender(){
    Fixture f;Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    assert(f.enqueue(300,home,0));
    const uint8_t data[]={8,0x80,0x02,18,1,'x',72,0};
    assert(f.service.submitPublicApplication(0,UINT32_MAX,301,data,sizeof(data),0,false,100)==0);
    uint32_t at=100;
    for(;at<=6000&&f.radio.frames.empty();at+=100) {
        assert(f.clock.set(at));f.service.poll(at,65);
    }
    assert(f.service.active()&&f.radio.frames.size()==1&&bench::le32(f.radio.frames[0].data()+8)==301);
    assert(!f.radio.busy());f.poll(at);
    assert(f.radio.scans==1&&f.runtime.queued()==1&&!f.runtime.radioHeld());
    f.radio.complete=true;f.service.poll(at,65);assert(!f.service.active()&&!f.service.faulted());
    f.poll(++at);f.frame(1,300,"LongFast");f.finish(++at);
}
static void preservedHomeRx(){
    Fixture f(true);Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    assert(f.enqueue(400,home,0));f.radio.blockScan=true;draw=1;f.poll(10);
    assert(f.runtime.radioHeld()&&f.radio.frames.empty());
    f.radio.blockScan=false;f.radio.rxPending=true;
    auto at=10+f.delay(draw);f.poll(at);
    assert(f.runtime.homeRxPending()&&f.radio.scans==1&&f.radio.frames.empty());

    // The main-loop home drain delivers the preserved frame before notifying
    // the excursion of accepted routing work. Off-profile callbacks stay isolated.
    bench::Frame incoming{};
    size_t n=0;
    assert(f.service.codec.encodeZeroHopData(0xabcdf001,UINT32_MAX,401,f.plain.data(),f.plain.size(),
                                             incoming.bytes,sizeof(incoming.bytes),n));
    incoming.bytes[12]=(3u<<5)|2u;bool accepted=false;
    f.radio.rxPending=false;f.service.rx(incoming.bytes,n,2,at,1,true,&accepted);
    assert(f.service.received==1&&accepted);f.runtime.receiveAccepted();f.poll(++at);
    assert(f.radio.frames.empty()&&!f.runtime.busy());
}
static void pendingHomeAck(){
    Fixture f;Excursion::ResolvedTarget home{};home.profile=Runtime::runningProfile(f.home);
    assert(f.enqueue(450,home,0));f.radio.blockScan=true;draw=3;f.poll(10);
    f.radio.blockScan=false;f.radio.rxPending=true;assert(f.runtime.homeRxPending());
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;d.has_bitfield=true;
    d.payload.size=2;memcpy(d.payload.bytes,"hi",2);
    uint8_t data[32];auto stream=pb_ostream_from_buffer(data,sizeof(data));
    assert(pb_encode(&stream,meshtastic_Data_fields,&d));
    bench::Frame incoming;size_t n=0;const uint32_t peer=0xabcdf002;
    assert(f.service.codec.encodeZeroHopData(peer,Self,451,data,stream.bytes_written,
                                             incoming.bytes,sizeof(incoming.bytes),n));
    incoming.bytes[12]=8;bool routed=true;
    f.radio.rxPending=false;f.service.rx(incoming.bytes,n,2,20,700,true,&routed);
    assert(!routed&&f.service.endpointControls()==1);
    f.poll(21);assert(!f.runtime.radioHeld()&&!f.runtime.busy()&&f.radio.frames.empty());
    assert(f.clock.set(400));f.service.poll(400,65);
    assert(f.radio.frames.size()==1);
    const auto &frame=f.radio.frames.front();bench::RouteMetadata meta{};
    assert(bench::le32(frame.data())==peer&&f.service.codec.decode(frame.data(),frame.size(),meta));
    assert(meta.portnum==meshtastic_PortNum_ROUTING_APP&&meta.request_id==451);
    f.radio.complete=true;f.service.poll(401,65);
}
static void boundedAdmission(bool recover,bool unsupported=false){
    Fixture f;draw=255;auto target=f.resolve(meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW);
    f.service.channelMeter().observeRx(60000,true);meterOwner::nativeSupport=!unsupported;
    const uint32_t start=recover?59000u:100u;
    assert(f.enqueue(500,target,start));f.poll(start);
    const auto delay=f.delay(draw);assert(delay>5000&&f.radio.isolated&&f.radio.scans==0);
    f.poll(start+4999);assert(f.runtime.radioHeld()&&f.radio.scans==0);
    f.poll(start+5000);
    if(recover) {
        // Initial 100% credit aged out. Keep the already drawn, now-admissible delay.
        assert(f.runtime.radioHeld());f.poll(start+delay-1);assert(!f.radio.scans);
        f.poll(start+delay);f.frame(0,500,"LongSlow");f.finish(start+delay+1);
    }
    assert(!f.runtime.busy()&&!f.radio.isolated&&f.radio.frames.size()==unsigned(recover));
}
#include "admin.inc"
int main(){
    offChannel();reusedSlots();changedHome();homeRestoreRetry();delayedSplit();cancelInFlight();faultRecovery();
    retiredTarget();terminalSignal();normalSender();preservedHomeRx();pendingHomeAck();stagedAdminRead();boundedAdmission(false);boundedAdmission(true);boundedAdmission(false,true);
    std::puts("{\"target_rechecks\":2,\"fifo_frames\":4,\"runtime_scenarios\":19,\"refilled_slots\":24}");
}
