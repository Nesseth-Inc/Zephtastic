// SPDX-License-Identifier: GPL-3.0-only
#include "meshtastic/config.pb.h"
#include "stock/ui/HeadlessButton.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using Commands=mt::stock::ui::SystemCommandsModule;
using Action=Commands::Action;
namespace ref {
#include "events.inc"
struct InputEvent {unsigned inputEvent, kbchar;};
struct {meshtastic_Config_DeviceConfig device{};meshtastic_Config_PositionConfig position{};meshtastic_Config_BluetoothConfig bluetooth{};} config;
struct {struct{bool enabled=true;}external_notification;} moduleConfig;
struct {bool muted=false;bool getMute(){return muted;}void setMute(bool b){muted=b;}void stopNow(){}} notify,*externalNotificationModule=&notify;
Action recorded=Action::None;
struct {
    void increaseBrightness(){recorded=Action::Brighten;}
    void decreaseBrightness(){recorded=Action::Dim;}
    void showSimpleBanner(const char*,int){}
    void forceDisplay(){}
} panel,*screen=&panel;
struct {void clearLocalPosition(){} void saveToDisk(int=0){} void factoryReset(bool){recorded=Action::FactoryReset;}} database,*nodeDB=&database;
struct {void saveToFlash(){}} messageStore;
struct {void toggleGpsMode(){config.position.gps_mode=meshtastic_Config_PositionConfig_GpsMode_DISABLED;}} receiver,*gps=&receiver;
struct {void refreshLocalMeshNode(){} bool trySendPosition(uint32_t,bool){recorded=Action::Ping;return false;}} mesh,*service=&mesh;
struct Time{static int timerEndsAtMillis(int delay){return 91+delay;}static int skipZero(int v){return v?v:1;}static int getMillis(){return 91;}};
void disableBluetooth(){}
int rebootAtMsec,shutdownAtMsec;
constexpr int DEFAULT_REBOOT_SECONDS=1, SEGMENT_CONFIG=0;
constexpr uint32_t NODENUM_BROADCAST=UINT32_MAX;
#define HAS_SCREEN 1
#define LOG_INPUT(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define IF_SCREEN(...) __VA_ARGS__
struct SystemCommandsModule{static int handleInputEvent(const InputEvent*);};
#include "commands.inc"
Action process(unsigned code,unsigned key){
    recorded=Action::None;notify.muted=false;
    config.bluetooth.enabled=true;config.position.gps_mode=meshtastic_Config_PositionConfig_GpsMode_ENABLED;
    config.device.buzzer_mode=meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED;
    rebootAtMsec=shutdownAtMsec=0;
    InputEvent event{code,key};SystemCommandsModule::handleInputEvent(&event);
    if(recorded!=Action::None)return recorded;
    if(!config.bluetooth.enabled)return Action::Bluetooth;
    if(notify.muted)return Action::Mute;
    if(rebootAtMsec)return Action::Reboot;
    if(shutdownAtMsec)return Action::Shutdown;
    if(config.device.buzzer_mode==meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED)return Action::Privacy;
    if(config.position.gps_mode==meshtastic_Config_PositionConfig_GpsMode_DISABLED)return Action::Gps;
    return Action::None;
}
struct Mapping {unsigned singlePress,longPress,longPressTime,longLongPress,doublePress,triplePress;};
Mapping headless(){
    Mapping userConfigNoScreen{};
#include "headless.inc"
    return userConfigNoScreen;
}
}
int main(){
    unsigned cases=0;
    for(unsigned code=0;code<256;++code)for(unsigned key=0;key<256;++key){
        assert(ref::process(code,key)==Commands::decode({uint8_t(code),uint8_t(key)}));
        ++cases;
    }
    std::printf("SystemCommandsModule %u event/key comparisons\n",cases);
    const auto stock=ref::headless();
    using Button=mt::stock::ui::HeadlessButton;
    for(unsigned taps:{1u,2u,3u}){
        Button actual;
        unsigned activities=0;
        auto sample=[&](bool down,unsigned t){return actual.update(down,t,[&]{++activities;});};
        sample(false,30000);
        uint32_t time=30100;
        for(unsigned tap=0;tap<taps;++tap){
            sample(true,time);sample(true,time+31);sample(false,time+65);sample(false,time+96);time+=140;
        }
        const unsigned refEvent=taps==1?stock.singlePress:taps==2?stock.doublePress:stock.triplePress;
        Action expected=refEvent==ref::INPUT_BROKER_USER_PRESS?Action::UserPress:ref::process(refEvent,0);
        assert(activities==taps&&sample(false,time+300)==expected);
    }
    for(unsigned hold:{650u,4100u}){
        Button actual;
        const uint32_t time=31000;
        auto sample=[&](bool down,unsigned t){return actual.update(down,t,[]{});};
        sample(false,30000);sample(true,time);sample(true,time+31);sample(false,time+hold);
        assert(sample(false,time+hold+31)==ref::process(hold<3900?stock.longPress:stock.longLongPress,0));
    }
    std::puts("InputBroker 5 stock headless button mappings");
}
