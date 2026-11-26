// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d: src/modules/SystemCommandsModule.cpp and src/input/InputBroker.h.
#pragma once
#include <stdint.h>

namespace mt::stock::ui {
struct SystemCommandsModule {
    enum class Action : uint8_t {None, Brighten, Dim, Mute, Bluetooth, Reboot, Gps, Privacy, Ping, Shutdown, FactoryReset, UserPress};
    struct Event {uint8_t code, key;};
    static constexpr Action decode(Event e) {
        switch(e.key){
        case 0xf1:case 0xf2:return Action::None;
        case 0x11:return Action::Brighten;
        case 0x12:return Action::Dim;
        case 0xac:return Action::Mute;
        case 0xaa:return Action::Bluetooth;
        case 0x90:return Action::Reboot;
        }
        switch(e.code){
        case 0x9e:return Action::Gps;
        case 0x9f:return Action::Privacy;
        case 0xaf:return Action::Ping;
        case 0x9b:return Action::Shutdown;
        case 0x9a:return Action::FactoryReset;
        default:return Action::None;
        }
    }
    struct Item {const char *name;Event event;};
    // T096's single local key selects the same commands as a keyboard hotkey.
    static constexpr Item Menu[]={
        {"Back",{}},{"Mesh ping",{0xaf,0}},{"Toggle GPS",{0x9e,0}},{"Toggle privacy",{0x9f,0}},
        {"Mute / unmute",{0,0xac}},{"Toggle Bluetooth",{0,0xaa}},{"Brightness +",{0,0x11}},
        {"Brightness -",{0,0x12}},{"Reboot",{0,0x90}},{"Shut down",{0x9b,0}},
        {"Factory reset device",{0x9a,0}}
    };
    uint8_t row=0;
    bool open=false,confirm=false,reported=false,deviceReset=false;
    int result=0;
    uint32_t lastInput=0;
    void begin(uint32_t now,bool allowDeviceReset=false){*this={};open=true;deviceReset=allowDeviceReset;lastInput=now;}
    void move(bool next,uint32_t now){
        const unsigned count=sizeof(Menu)/sizeof(Menu[0])-(deviceReset?0:1);
        row=uint8_t((row+(next?1:count-1))%count);confirm=false;reported=false;lastInput=now;
    }
    template<class Run> void select(uint32_t now,Run run){
        lastInput=now;
        if(!row){open=false;return;}
        if(!confirm){confirm=true;reported=false;return;}
        confirm=false;reported=true;result=run(decode(Menu[row].event),now);
    }
    static constexpr uint8_t brightness(uint8_t value,bool up){
        return up?(value>192?value:uint8_t(value+62)):(value<70?value:uint8_t(value-62));
    }
};
}
