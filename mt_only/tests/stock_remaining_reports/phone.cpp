// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "ModuleController.h"
#include "stock/modules/PositionModule.h"

namespace positionDonor {
struct PositionModule {
    bool sendOurPositionToPhone();
    static bool shouldSendPositionToPhone(bool,bool,bool,uint32_t,uint32_t,uint32_t);
    unsigned precision=0;bool allocation=true;
    meshtastic_MeshPacket packet{};
    meshtastic_MeshPacket *allocPositionPacket(unsigned p){precision=p;return allocation?&packet:nullptr;}
};
struct {struct {bool fixed_position;} position;} config;
struct {bool fresh=false;bool hasLocalPositionSinceBoot(){return fresh;}} nodeState,*nodeDB=&nodeState;
struct {meshtastic_MeshPacket *sent=nullptr;void sendToPhone(meshtastic_MeshPacket *p){sent=p;}} queue,*service=&queue;
constexpr uint32_t NODENUM_BROADCAST=UINT32_MAX;
#include "phone-position.inc"
}
namespace deviceDonor {
struct MeshModule {unsigned numPeriodicModules=0;int32_t setStartDelay();};
#include "start.inc"
uint32_t now,meshStamp;

struct Time {static uint32_t getMillis(){return now;}static uint32_t skipZero(uint32_t n){return n?n:1;}};
struct Throttle {static bool hasElapsed(uint32_t a,uint32_t b){return uint32_t(now-a)>=b;}};
enum class TrafficType {TELEMETRY};
struct Default {static uint32_t getConfiguredOrDefaultMsScaled(uint32_t,uint32_t,uint32_t,TrafficType){return 3600000;}};
constexpr uint32_t TX_HISTORY_KEY_DEVICE_TELEMETRY=0x8000,NODENUM_BROADCAST=UINT32_MAX;
constexpr unsigned default_telemetry_broadcast_interval_secs=3600,numOnlineNodes=1;
struct {uint32_t getLastSentToMeshMillis(uint32_t){return meshStamp;}void setLastSentToMesh(uint32_t){meshStamp=now;}} history,*transmitHistory=&history;
struct {bool isTxAllowedChannelUtil(bool){return true;}bool isTxAllowedAirUtil(){return true;}} meter,*airTime=&meter;
struct {struct {unsigned role;} device;} config;
struct {struct {bool device_telemetry_enabled=false;unsigned device_update_interval=0;} telemetry;} moduleConfig;
struct {bool empty=false;bool isToPhoneQueueEmpty(){return empty;}} queue,*service=&queue;
struct DeviceTelemetryModule {
    uint32_t lastSentStatsToPhone=0,sendToPhoneIntervalMs=60000,sendStatsToPhoneIntervalMs=900000;
    std::vector<unsigned> sent;bool mesh=false;
    bool isSensorOrRouterRole(){return false;}
    void sendTelemetry(uint32_t dest=NODENUM_BROADCAST,bool phone=false){
        assert(dest==NODENUM_BROADCAST);mesh=!phone;sent.push_back(meshtastic_Telemetry_device_metrics_tag);
    }
    void sendLocalStatsToPhone(){sent.push_back(meshtastic_Telemetry_local_stats_tag);}
    int32_t runOnce();
};
#include "phone-device.inc"
}

class Phone:public Host {
public:
    using Host::Host;
    bool allowed=true,blocked=false,working=true,edit=false;
    Security security{true,false,0,2,0};
    bool consoleAllowed()const override{return allowed;}
    bool ready()const override{return working;}
    bool editing()const override{return edit;}
    Security bluetoothSecurity()const override{return security;}
    bool sendIfQueueEmpty(const uint8_t *p,size_t n)override{
        if(blocked||!out.empty())return false;
        return send(p,n);
    }
};
struct Fanout {
    mt::Companion &companion,&bleCompanion,&serialCompanion,&tcpCompanion;
    mt::Companion::WorkspaceProof companionWorkspaceProof(){return mt::Companion::WorkspaceProof{true};}
#define CONFIG_MT_SERIAL_MODULE 1
#define CONFIG_MT_TCP_COMPANION 1
#include "fanout.inc"
#undef CONFIG_MT_SERIAL_MODULE
#undef CONFIG_MT_TCP_COMPANION
};
static void connect(mt::Companion &c,Phone &h,unsigned nonce=1){
    meshtastic_ToRadio req{};req.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
    req.want_config_id=nonce;input(c,req,10);h.out.clear();
}
static std::vector<unsigned> reports(const Phone &h,bool background=true){
    std::vector<unsigned> result;
    for(const auto &msg:h.out){
        const auto &packet=msg.packet;
        assert(msg.which_payload_variant==meshtastic_FromRadio_packet_tag&&packet.from==OURS&&packet.to==UINT32_MAX);
        if(background)assert(packet.priority==meshtastic_MeshPacket_Priority_BACKGROUND);
        assert(packet.id&&packet.decoded.portnum==meshtastic_PortNum_TELEMETRY_APP&&!packet.decoded.want_response);
        meshtastic_Telemetry telemetry{};
        auto in=pb_istream_from_buffer(packet.decoded.payload.bytes,packet.decoded.payload.size);
        assert(pb_decode(&in,meshtastic_Telemetry_fields,&telemetry)&&!in.bytes_left);result.push_back(telemetry.which_variant);
    }
    return result;
}
static void localStats(Fixture &f,Phone &usb,Phone &ble,Fanout phones,unsigned role,bool enabled){
    f.s.stop();auto cfg=f.s.config;cfg.setTxEnabled(true);assert(cfg.setDeviceRole(role,0));
    assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
    mt::ModuleController controller;mt::ModuleSettings settings;
    meshtastic_ModuleConfig config{};config.which_payload_variant=meshtastic_ModuleConfig_telemetry_tag;
    config.payload_variant.telemetry.device_telemetry_enabled=enabled;
    config.payload_variant.telemetry.device_update_interval=3600;assert(settings.set(config));
    deviceDonor::moduleConfig.telemetry.device_telemetry_enabled=enabled;
    deviceDonor::meshStamp=0;deviceDonor::DeviceTelemetryModule donor;
    deviceDonor::MeshModule registration;
    (void)registration.setStartDelay(); // NodeInfo and MeshBeacon
    (void)registration.setStartDelay();
    if(role!=meshtastic_Config_DeviceConfig_Role_TRACKER)(void)registration.setStartDelay();
    (void)registration.setStartDelay(); // KeyVerification
    uint32_t deadline=registration.setStartDelay();const auto first=deadline;
    auto tick=[&](uint32_t now){
        f.channelNow=now;usb.out.clear();ble.out.clear();
        usb.blocked=now==first+120000;ble.blocked=now!=first+60000;
        donor.sent.clear();donor.mesh=false;
        if(now>=deadline){
            deviceDonor::queue.empty=!usb.blocked;deviceDonor::now=now;
            deadline=now+donor.runOnce();
        }
        controller.pollReports(f.s,settings,true,now,6789,0,[&](const uint8_t *p,size_t n,uint32_t id,
                           mt::ModuleController::PhoneDelivery delivery,uint8_t recipients){
            using D=mt::ModuleController::PhoneDelivery;
            return phones.publishModuleData(p,n,id,0,delivery==D::Idle||delivery==D::DeviceIdle,recipients,
                                            delivery==D::DeviceIdle||delivery==D::FollowUp);
        });
        assert(reports(usb,!donor.mesh)==donor.sent);
        if(donor.mesh){
            assert(enabled&&now==first&&f.s.endpointControls()==1&&reports(ble,false)==donor.sent);
            f.s.stop();assert(f.s.start(false,now)&&f.s.armEndpoint(true));
        }else{
            assert(!f.s.endpointControls());
            if(ble.blocked)assert(ble.out.empty());
            else assert(reports(ble)==reports(usb));
        }
        assert(!f.radio.sends);
    };
    for(uint32_t now=0;now<=1020000;now+=15000)tick(now);
    const auto delayed=deadline+59999;
    tick(delayed);tick(delayed+1);tick(delayed+59999);tick(delayed+60000);
}
static void positions(Fixture &f,Phone &usb,Phone &ble,Fanout phones){
    for(auto role:{meshtastic_Config_DeviceConfig_Role_CLIENT,meshtastic_Config_DeviceConfig_Role_TRACKER,
                   meshtastic_Config_DeviceConfig_Role_TAK_TRACKER}){
        mt::stock::position::PhonePositionPoll scheduler;
        deviceDonor::MeshModule registration;
        (void)registration.setStartDelay(); // NodeInfo
        (void)registration.setStartDelay(); // MeshBeacon
        const uint32_t start=UINT32_MAX-35000;
        uint32_t boundary=role==meshtastic_Config_DeviceConfig_Role_CLIENT?registration.setStartDelay():0;
        for(uint32_t offset=0;offset<=75000;offset+=1000){
            const bool expected=offset==boundary;
            if(expected)boundary+=5000;
            assert(scheduler.due(start+offset,role)==expected);
        }
    }
    constexpr int32_t lat=431234567,lon=-921234567;
    const mt::Companion::WorkspaceProof proof{true};
    f.s.stop();auto cfg=f.s.config;
    assert(cfg.setFixedPosition(lat,lon,true,437)&&!cfg.positionSharing()&&f.s.apply(cfg)&&f.s.start(false,0));
    positionDonor::PositionModule ref;positionDonor::config.position.fixed_position=true;
    usb.out.clear();ble.out.clear();usb.blocked=ble.blocked=false;
    bool sent=false;uint32_t last=0;
    for(uint32_t now:{UINT32_MAX-32000,UINT32_MAX-31000,0u,28998u,28999u,29000u,90000u}){
        usb.out.clear();usb.blocked=now==UINT32_MAX-32000;
        bool expected=ref.shouldSendPositionToPhone(true,!usb.blocked,sent,now,last,60000);
        if(expected){assert(ref.sendOurPositionToPhone());sent=true;last=now;}
        assert(phones.companion.sendLocalPosition(now,proof)==expected);
        if(expected){
            const auto &p=usb.out.at(0).packet;
            assert(ref.precision==32&&p.priority==ref.packet.priority&&p.to==ref.packet.to&&!p.decoded.want_response);
            assert(p.from==OURS&&p.id&&p.decoded.portnum==meshtastic_PortNum_POSITION_APP);
            meshtastic_Position pos{};auto in=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
            assert(pb_decode(&in,meshtastic_Position_fields,&pos)&&pos.precision_bits==32);
            assert(pos.latitude_i==lat&&pos.longitude_i==lon);
        }else assert(usb.out.empty());
    }
    for(auto security:{Phone::Security{false,false,0,2,0},Phone::Security{true,false,0,1,0},
                       Phone::Security{true,true,0,2,0},Phone::Security{true,false,0,2,1}}){
        ble.security=security;assert(!phones.bleCompanion.sendLocalPosition(100,proof));
    }
    ble.security={true,false,0,2,0};assert(phones.bleCompanion.sendLocalPosition(100,proof));
    ble.out.clear();assert(!phones.bleCompanion.sendLocalPosition(101,proof));
    assert(!phones.serialCompanion.sendLocalPosition(200000,proof)&&!phones.tcpCompanion.sendLocalPosition(200000,proof));
    usb.allowed=false;assert(!phones.companion.sendLocalPosition(200000,proof));usb.allowed=true;
    usb.working=false;assert(!phones.companion.sendLocalPosition(200000,proof));usb.working=true;
    usb.edit=true;assert(!phones.companion.sendLocalPosition(200000,proof));usb.edit=false;
    phones.companion.resetStream();assert(!phones.companion.sendLocalPosition(200000,proof));
    connect(phones.companion,usb,69420);assert(!phones.companion.sendLocalPosition(200000,proof));
    connect(phones.companion,usb);
    assert(!phones.companion.sendLocalPosition(90001,proof)); // A handshake does not reset cadence.
    f.s.stop();cfg.clearFixedPosition();auto gps=cfg.gnssSettings();gps.enabled=true;
    assert(cfg.setGnssSettings(gps)&&f.s.apply(cfg)&&f.s.start(false,0));
    positionDonor::config.position.fixed_position=false;assert(!ref.sendOurPositionToPhone());
    assert(!phones.companion.sendLocalPosition(200000,proof)); // No fix this boot.
    meshtastic_Position live{};live.has_latitude_i=live.has_longitude_i=true;live.latitude_i=lat;live.longitude_i=lon;
    f.s.measuredPosition(live);positionDonor::nodeState.fresh=true;
    assert(ref.sendOurPositionToPhone()&&phones.companion.sendLocalPosition(200000,proof));
    f.s.measuredPosition({});assert(!phones.companion.sendLocalPosition(300000,proof));
    assert(!f.s.endpointControls()&&!f.radio.sends);
}
void phoneCases(){
    for(auto role:{meshtastic_Config_DeviceConfig_Role_CLIENT,meshtastic_Config_DeviceConfig_Role_TRACKER})
        for(bool enabled:{false,true}){
            auto fixture=std::make_unique<Fixture>();auto &f=*fixture;
            Phone a(f.s),b(f.s),c(f.s),d(f.s);
            mt::Companion usb(f.s,a,81,f.workspace,0),ble(f.s,b,81,f.workspace,1),
                serial(f.s,c,81,f.workspace,3),tcp(f.s,d,81,f.workspace,2);
            for(auto pair:{std::make_pair(&usb,&a),std::make_pair(&ble,&b),
                           std::make_pair(&serial,&c),std::make_pair(&tcp,&d)})
                connect(*pair.first,*pair.second);
            c.blocked=d.blocked=true;
            Fanout phones{usb,ble,serial,tcp};
            localStats(f,a,b,phones,role,enabled);
            if(!enabled&&role==meshtastic_Config_DeviceConfig_Role_CLIENT)positions(f,a,b,phones);
        }
}
