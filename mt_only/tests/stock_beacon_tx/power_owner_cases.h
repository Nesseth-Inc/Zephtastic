// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "power/PowerPolicy.h"

int64_t k_uptime_get(){return 35000;}
namespace mt::power::ina {bool supports(uint32_t address){return address==0;}}

namespace beaconPower {
using Policy=mt::power::PowerPolicy;
enum class Action { TimedSleep, BatteryShutdown, RequestedShutdown };

struct Backend {
    Fixture &fixture;
    unsigned prepared=0,entered=0;

    static bool prepare(void *ctx){
        auto &b=*static_cast<Backend *>(ctx);
        ++b.prepared;
        return b.fixture.service.storageStopSafe()&&b.fixture.service.receiving();
    }
    static bool pause(void *ctx){
        auto &b=*static_cast<Backend *>(ctx);
        return prepare(ctx)&&b.fixture.service.pauseForStorage()==0;
    }
    static int enter(void *ctx){++static_cast<Backend *>(ctx)->entered;return -EIO;}
    static bool off(void *ctx){return static_cast<Backend *>(ctx)->fixture.service.storageQuiescent();}
    static void resume(void *){}
};

static void exercise(Action action){
    Fixture f;
    Backend b{f};
    Policy policy;
    policy.setDeepSleepBackend({&b,[](void *,uint32_t seconds){return seconds>0;},Backend::prepare,
        [](void *){return true;},[](void *ctx,uint32_t){Backend::enter(ctx);return 0;},Backend::resume});
    policy.setPowerOffBackend({&b,[](void *){return true;},Backend::pause,Backend::off,Backend::enter,Backend::resume});
    meshtastic_Config_PowerConfig config{};
    config.is_power_saving=action==Action::TimedSleep;
    config.on_battery_shutdown_after_secs=action==Action::BatteryShutdown?30:0;
    config.wait_bluetooth_secs=config.min_wake_secs=config.ls_secs=1;
    assert(policy.configure(config,0));
    Policy::Activity idle{};
    idle.externalPowerKnown=true;
    if(action==Action::TimedSleep){
        assert(policy.poll(1000,idle)==Policy::State::LightSleepEligible);
        assert(policy.poll(35000,idle)==Policy::State::DeepSleepPending);
    }else if(action==Action::BatteryShutdown){
        assert(policy.poll(35000,idle)==Policy::State::BatteryShutdownPending);
    }

    Excursion::ResolvedTarget home{};
    home.profile=Runtime::runningProfile(f.home);
    assert(f.enqueue(622,home,0));
    f.poll(10);
    assert(f.runtime.radioHeld()&&f.radio.borrowed&&!f.radio.busy());
    // Run the actual owner-loop branch immediately after beaconTargets.poll().
    auto &beaconTargets=f.runtime;
    int serviceLock=0,watchdog=0,channel=0;
    unsigned unlocks=0,feeds=0,waits=0,attempts=0;
    auto k_mutex_unlock=[&](int *){++unlocks;};
    auto wdt_feed=[&](int,int){++feeds;return 0;};
    auto K_MSEC=[](int ms){return ms;};
    auto k_sleep=[&](int ms){assert(ms==5);++waits;};
    auto pass=[&]{
        do {
#include "power-owner-acquisition.inc"
            ++attempts;
            if(action==Action::TimedSleep)
                assert(policy.executeDeepSleep(idle)==Policy::DeepSleepResult::Resumed);
            else {
                auto result=action==Action::BatteryShutdown?policy.executeBatteryShutdown(idle):
                    policy.executeRequestedPowerOff(idle,true);
                // The instrumented power-off backend returns so the fixture can inspect it.
                assert(result==Policy::DeepSleepResult::BackendFailed);
            }
        }while(false);
    };
    pass();
    assert(unlocks==1&&feeds==1&&waits==1&&attempts==0&&b.prepared==0&&b.entered==0);
    assert(f.radio.borrowed&&f.radio.frames.size()==1&&!f.service.faulted());
    f.finish(11);
    assert(!f.runtime.radioHeld()&&!f.radio.borrowed&&f.service.receiving());
    pass();
    assert(attempts==1&&b.prepared==1&&b.entered==1&&!f.service.faulted());
}
static unsigned cases(){
    unsigned checked=0;
    for(auto action:{Action::TimedSleep,Action::BatteryShutdown,Action::RequestedShutdown}){
        exercise(action);
        ++checked;
    }
    return checked;
}
}
