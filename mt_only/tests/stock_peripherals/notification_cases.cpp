// SPDX-License-Identifier: GPL-3.0-only
#include "notifications/ExtendedNotificationController.h"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
struct Hardware {
    unsigned tones=0, starts=0, stops=0;
    uint16_t frequency=0;
    bool playing=false;
    static bool pwm(void *ctx,uint16_t freq,uint8_t){
        auto &h=*static_cast<Hardware*>(ctx);h.frequency=freq;if(freq)++h.tones;return true;
    }
    static bool i2s(void *ctx,const char *,size_t,bool begin){
        auto &h=*static_cast<Hardware*>(ctx);if(begin){++h.starts;h.playing=true;}return h.playing;
    }
    static void stop(void *ctx){auto &h=*static_cast<Hardware*>(ctx);++h.stops;h.playing=false;}
};
}
void notificationCases()
{
    using namespace mt::notification;
    static const char single[]="test:d=4,o=5,b=120:c";
    uint8_t payload[1]={'x'};
    for(int kind=0;kind<2;++kind) {
        Hardware hw;
        ExtendedNotificationBackend backend{};
        backend.context=&hw;backend.setPwmTone=Hardware::pwm;backend.playRtttl=Hardware::i2s;backend.stopRtttl=Hardware::stop;
        ExtendedNotificationCapabilities caps{};caps.pwmBuzzer=kind==0;caps.i2sRtttl=kind==1;
        ExtendedNotificationConfig cfg{};cfg.enabled=true;cfg.outputMs=20;cfg.alertMessageBuzzer=true;
        cfg.usePwm=caps.pwmBuzzer;cfg.useI2s=caps.i2sRtttl;
        ExtendedNotificationController c;
        assert(c.configure(cfg,caps,backend));
        assert(c.setRingtone(single,strlen(single)));
        const uint32_t start=UINT32_MAX-10;
        assert(c.receive(2,1,2,meshtastic_PortNum_TEXT_MESSAGE_APP,payload,1,false,false,start));
        c.poll(start);
        assert((!kind&&hw.tones==1&&hw.frequency)||(kind&&hw.starts==1));
        assert(c.setRingtone(single,strlen(single)));
        c.poll(start+20);
        assert(c.armed());
        if(kind)hw.playing=false;
        c.poll(start+500);
        assert(!c.armed() && !hw.frequency && !hw.playing && hw.starts==unsigned(kind));
        assert(c.receive(2,1,3,meshtastic_PortNum_TEXT_MESSAGE_APP,payload,1,false,false,start+1000));
        c.poll(start+1000);
        assert(mt::stock::notification::handleInputEvent(c.armed(),[&]{c.stop();}));
        assert(!mt::stock::notification::handleInputEvent(c.armed(),[&]{c.stop();}));
        assert(!c.armed()&&!hw.playing&&!hw.frequency);
    }
    puts("PASS production extended notification PWM/I2S, last-note cutoff across rollover, stable ringtone and one-input dismissal");
}
