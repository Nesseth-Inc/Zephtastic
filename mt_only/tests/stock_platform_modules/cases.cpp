// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "stock/platform/PowerStressModule.h"
#include "stock/platform/WatchdogThread.h"
#include "stock/telemetry/ReportModules.h"
#include <limits>
#include "stock/optional/FixedDecimal.h"

namespace donor {
uint32_t now=0,uptime=0;
uint32_t getTime(){return now;}
struct Time { static uint32_t getUptimeSecs(){return uptime;} };
struct Air {
    float air=0,channel=0;
    float utilizationTXPercent()const{return air;}
    float channelUtilizationPercent()const{return channel;}
} air;
Air *airTime=&air;
struct Power {
    int mv=3300,percent=50;
    bool battery=true,charging=false;
    bool getHasBattery(){return battery;}
    bool getIsCharging(){return charging;}
    int getBatteryChargePercent(){return percent;}
    int getBatteryVoltageMv(){return mv;}
} power;
Power *powerStatus=&power;
constexpr unsigned MAGIC_USB_BATTERY_LEVEL=101;
struct DeviceTelemetryModule {static meshtastic_Telemetry getDeviceTelemetry();};
struct EnvironmentTelemetryModule {static bool getEnvironmentTelemetry(meshtastic_Telemetry *);};
struct PowerTelemetryModule {static bool getPowerTelemetry(meshtastic_Telemetry *);};
struct TelemetrySensor {
    bool present=false;
    bool hasSensor()const{return present;}
    bool getMetrics(meshtastic_Telemetry *t) {
        if(!present)return false;
        if(t->which_variant==meshtastic_Telemetry_environment_metrics_tag) {
            auto &m=t->variant.environment_metrics;
            m.has_temperature=true;m.temperature=0;
        } else {
            auto &m=t->variant.power_metrics;
            m.has_ch1_voltage=true;m.ch1_voltage=3.5f;
        }
        return true;
    }
} ina219Sensor,ina226Sensor,ina260Sensor,ina3221Sensor,max17048Sensor,environment;
TelemetrySensor *sensors[]={&environment};
#define HAS_TELEMETRY 1
#define T1000X_SENSOR_EN 1
struct WatchdogThread {static void feedDog();static bool setup();};
constexpr int INPUT=0,OUTPUT=1,LOW=0,HIGH=1,HARDWARE_WATCHDOG_DONE=1,HARDWARE_WATCHDOG_WAKE=2;
std::vector<int> events;
void pinMode(int pin,int mode){events.push_back(100+pin*10+mode);}
void digitalWrite(int pin,int level){assert(pin==HARDWARE_WATCHDOG_DONE);events.push_back(level);}
void delay(unsigned ms){assert(ms==1);events.push_back(3);}
#define LOG_DEBUG(...) ((void)0)
enum class ProcessMessage { CONTINUE };
struct AtakPluginModule {static ProcessMessage handleReceived(const meshtastic_MeshPacket &);};
#include "stock.inc"
}
static std::vector<uint8_t> bytes(const meshtastic_Telemetry &value){
    uint8_t buffer[233];auto stream=pb_ostream_from_buffer(buffer,sizeof(buffer));
    assert(pb_encode(&stream,meshtastic_Telemetry_fields,&value));return {buffer,buffer+stream.bytes_written};
}
static void reports(){
    using namespace mt::stock::telemetry;
    for(uint32_t when:{0u,1700000000u,UINT32_MAX})for(uint32_t age:{0u,91u})
    for(float air:{0.f,1.25f,100.f})for(float channel:{0.f,8.5f,100.f})for(bool charging:{false,true}){
        donor::now=when;donor::uptime=age;donor::air.air=air;donor::air.channel=channel;donor::power.charging=charging;
        mt::HardwareSnapshot snapshot;snapshot.cellMilliVolts=3300;snapshot.cellPercent=50;snapshot.externalPower=charging;
        meshtastic_Telemetry t{};deviceReport(t,when,age,air,channel,snapshot);
        assert(bytes(t)==bytes(donor::DeviceTelemetryModule::getDeviceTelemetry()));
    }
    for(bool present:{false,true}){
        meshtastic_Telemetry ref{},candidate{};
        donor::now=123;donor::environment.present=present;
        zt::telemetry::EnvironmentReadings env{};if(present)env.ambientCelsius=0.f;
        assert(environmentReport(candidate,123,env)==donor::EnvironmentTelemetryModule::getEnvironmentTelemetry(&ref));
        assert(bytes(ref)==bytes(candidate));
        zt::telemetry::PowerReadings p{};if(present)p.voltage[0]=3.5f;donor::ina219Sensor.present=present;
        assert(powerReport(candidate,123,p)==donor::PowerTelemetryModule::getPowerTelemetry(&ref));
        assert(bytes(ref)==bytes(candidate));
    }
    for(unsigned role=0;role<=12;++role)for(int channel=0;channel<=100;++channel)for(bool duty:{false,true}){
        const bool sensor=role==meshtastic_Config_DeviceConfig_Role_SENSOR;
        const bool router=role==meshtastic_Config_DeviceConfig_Role_ROUTER || role==meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
        assert(deviceMeshAllowed(role,channel,duty)==(duty && role!=meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN && channel<(sensor||router?40:25)));
        assert(environmentMeshAllowed(role,channel,duty)==(duty && channel<(sensor?40:25)));
    }
}
static void watchdog(){
    std::vector<int> events;
    struct Pins {
        std::vector<int> &v;
        bool configure(){v.push_back(120);v.push_back(111);return true;}
        bool done(bool high){v.push_back(high);return true;}
        void delayMs(unsigned ms){assert(ms==1);v.push_back(3);}
    } pins{events};
    assert(donor::WatchdogThread::setup() && mt::stock::watchdog::setup(pins));assert(events==donor::events);
    donor::WatchdogThread::feedDog();assert(mt::stock::watchdog::feedDog(pins));assert(events==donor::events);
}
static meshtastic_ToRadio packet(uint32_t id,uint32_t to,meshtastic_PortNum port){
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    r.packet.id=id;r.packet.to=to;r.packet.hop_limit=3;r.packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    r.packet.decoded.portnum=port;return r;
}
static void bridge(){
    for(bool host:{false,true})for(unsigned owner:{0u,1u}){
        auto f=std::make_unique<Fixture>();auto r=packet(71,UINT32_MAX,host?meshtastic_PortNum_TELEMETRY_APP:meshtastic_PortNum_ATAK_PLUGIN_V2);
        std::vector<uint8_t> payload{0,9,0,2,250};
        if(host){
            meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_host_metrics_tag;t.variant.host_metrics.load1=25;
            payload=bytes(t);r.packet.priority=meshtastic_MeshPacket_Priority_BACKGROUND;
        }else assert(donor::AtakPluginModule::handleReceived(r.packet)==donor::ProcessMessage::CONTINUE);
        r.packet.decoded.payload.size=payload.size();memcpy(r.packet.decoded.payload.bytes,payload.data(),payload.size());
        input(owner?f->bc:f->c,r,100);f->s.poll(300,0);
        if(host && owner){assert(f->radio.frames.empty());continue;}
        assert(f->radio.frames.size()==1);
        bench::RouteMetadata m{};auto &tx=f->radio.frames[0];assert(f->s.codec.decode(tx.data(),tx.size(),m));
        assert(m.portnum==r.packet.decoded.portnum && m.payloadLength==payload.size() && !memcmp(m.payload,payload.data(),payload.size()));
    }
}
static void benchCommands(){
    using namespace mt::stock::powerstress;
    meshtastic_PowerStressMessage q{};q.cmd=meshtastic_PowerStressMessage_Opcode_CPU_FULLON;q.num_seconds=.25f;
    Module module;assert(module.receive(1,q)==0 && module.receive(1,q)==0);
    assert(module.poll(UINT32_MAX-9).remainingMs==250);
    assert(module.poll(9).remainingMs==231);
    q.cmd=meshtastic_PowerStressMessage_Opcode_BT_OFF;assert(module.receive(2,q)==-EBUSY);
    assert(module.poll(240).action==Action::Mark && module.poll(241).action==Action::None);
    assert(module.receive(2,q)==0 && module.poll(242).action==Action::BluetoothOff);
    q.num_seconds=std::numeric_limits<float>::quiet_NaN();assert(module.receive(3,q)==-EINVAL);
    auto f=std::make_unique<Fixture>();
    struct Usb:Host {
        unsigned calls=0;
        using Host::Host;
        int powerStress(uint32_t,const uint8_t*,size_t)override{++calls;return 0;}
    } usb(f->s),ble(f->s);
    mt::Companion a(f->s,usb,81,f->workspace,0),b(f->s,ble,81,f->workspace,1);
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=1;
    input(a,hello,1);input(b,hello,1);
    auto r=packet(83,OURS,meshtastic_PortNum_POWERSTRESS_APP);
    r.packet.decoded.payload.size=2;r.packet.decoded.payload.bytes[0]=8;r.packet.decoded.payload.bytes[1]=1;
    input(a,r,100);input(b,r,101);assert(usb.calls==1 && !ble.calls);
    r.packet.to=UINT32_MAX;input(a,r,102);f->s.poll(302,0);assert(usb.calls==1 && f->radio.frames.empty());
}
static void decimals(){
    for(uint32_t bits=1,i=0;i<10000;++i,bits=1664525u*bits+1013904223u){
        float value;memcpy(&value,&bits,4);
        for(unsigned places:{1u,2u}){
            char actual[48],expected[48];
            mt::stock::optional::fixedDecimal(value,places,actual);
            snprintf(expected,sizeof(expected),"%.*f",int(places),double(value));
            if(value!=value)assert(!strcmp(actual,"nan") || !strcmp(actual,"-nan"));
            else assert(!strcmp(actual,expected));
        }
    }
}
int main(){decimals();reports();watchdog();bridge();benchCommands();std::cout<<"PASS pinned report bytes, watchdog edges, host bridge and USB command isolation\n";}
