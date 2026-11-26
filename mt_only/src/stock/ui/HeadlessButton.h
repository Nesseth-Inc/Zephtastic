// SPDX-License-Identifier: GPL-3.0-only
// Pinned InputBroker's headless UserButton mapping, OneButton multi-click and post-boot long holdoff.
#pragma once
#include "SystemCommandsModule.h"
namespace mt::stock::ui {
struct HeadlessButton {
    bool raw=false,stable=false,initialized=false,armed=false,ignoreHeld=false;
    unsigned taps=0;
    uint32_t changed=0,pressed=0,released=0;
    using Action=SystemCommandsModule::Action;
    template<class Activity> Action update(bool down,uint64_t uptime,Activity activity){
        const uint32_t now=uint32_t(uptime);
        if(!initialized){raw=stable=down;changed=now;ignoreHeld=down;initialized=true;return Action::None;}
        if(down!=raw){raw=down;changed=now;}
        if(stable!=raw&&uint32_t(now-changed)>=30){
            stable=raw;
            if(ignoreHeld){if(!stable)ignoreHeld=false;return Action::None;}
            if(stable){pressed=now;armed=uptime>30000;activity();}
            else if(uint32_t(now-pressed)>=500){
                taps=0;
                if(armed&&uint32_t(now-pressed)>=3900){armed=false;return Action::Shutdown;}
            }else {released=now;if(taps<4)++taps;}
        }
        if(!raw&&!stable&&taps&&uint32_t(now-released)>=250){
            unsigned count=taps;taps=0;
            return count==2?Action::Ping:count==3?Action::Gps:count==1?Action::UserPress:Action::None;
        }
        return Action::None;
    }
};
}
