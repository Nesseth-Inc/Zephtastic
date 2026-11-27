// SPDX-License-Identifier: GPL-3.0-only
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "stock/platform/PowerStressModule.h"
#include "stock/platform/WatchdogThread.h"
#include "stock/peripherals/StatusLEDModule.h"
#include "stock/ui/BuzzerFeedback.h"
#include "stock/modules/RemoteHardwareModule.h"
#include <cassert>
#include <cstdio>
#include <vector>

// The test sinks below record the stock requests. No sleep, GPIO, audio, or
// Bluetooth backend is reachable.
namespace stock {
#include "events.inc"
struct InputEvent {unsigned inputEvent, kbchar;};
struct {struct {unsigned buzzer_mode;} device; struct {bool powermon_enables=true;} power;} config;
enum class Sound {None, Click, Beep, Chirp, Boop, Combo};
static Sound selected;
void playBeep(){selected=Sound::Beep;}
void playClick(){selected=Sound::Click;}
void playBoop(){selected=Sound::Boop;}
void playChirp(){selected=Sound::Chirp;}
void playComboTune(){selected=Sound::Combo;}
struct BuzzerFeedbackThread {static int handleInputEvent(const InputEvent*);};
#include "buzzer.inc"
struct EncoderFeedback {static int handleInputEvent(const InputEvent*);};
#define INPUTDRIVER_ENCODER_TYPE 1
#define BuzzerFeedbackThread EncoderFeedback
#include "buzzer.inc"
#undef BuzzerFeedbackThread
#undef INPUTDRIVER_ENCODER_TYPE

#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
uint32_t now=0;
uint32_t millis(){return now;}
enum {LED_LORA=3, LED_STATE_ON=1, LED_STATE_OFF=0, LOW=0, HIGH=1, INPUT=0, OUTPUT=1,
      HARDWARE_WATCHDOG_DONE=1, HARDWARE_WATCHDOG_WAKE=2};
#include "led-duration.inc"
bool led=false;
std::vector<unsigned> edges;
void digitalWrite(unsigned pin, unsigned value){
    if(pin==LED_LORA) led=value;
    else {assert(pin==HARDWARE_WATCHDOG_DONE);edges.push_back(value);}
}
void delay(unsigned ms){assert(ms==1);edges.push_back(3);}
void pinMode(int pin,int mode){edges.push_back(100+pin*10+mode);}
struct StatusLEDModule {
    bool LORA_LED_state=false;uint32_t LORA_LED_starttime=0;
    void setIntervalFromNow(unsigned value){assert(value==LORA_RX_LED_FLASH_MS);}
    int handleLoRaRx(uint32_t);
    void poll();
};
#include "led.inc"
struct WatchdogThread {static bool setup();static void feedDog();};
#include "watchdog.inc"

using Action=mt::stock::powerstress::Action;
Action called;
struct PowerMonitor {bool force_enabled=false;} monitor,*powerMon=&monitor;
void printInfo(){called=Action::Info;}
void setBluetoothEnable(bool enabled){called=enabled?Action::BluetoothOn:Action::BluetoothOff;}
void doDeepSleep(int ms,bool,bool){assert(ms==250);called=Action::Sleep;}
struct Throttle {static bool isWithinTimespanMs(uint32_t start,uint32_t duration){
    called=Action::Busy;return ++now-start<duration;
}};
struct PowerStressModule {
    meshtastic_PowerStressMessage currentMessage{};
    bool isRunningCommand=false;
    int32_t disable(){return -1;}
    bool handleReceivedProtobuf(const meshtastic_MeshPacket&,meshtastic_PowerStressMessage*);
    int32_t runOnce();
};
#include "power.inc"
}

static void buzzer(){
    using namespace stock;
    using Feedback=mt::stock::ui::BuzzerFeedback;
    unsigned cases=0;
    for(unsigned mode=0;mode<=4;++mode)for(bool encoder:{false,true})for(unsigned code=0;code<256;++code){
        InputEvent event{code,65};
        config.device.buzzer_mode=mode;selected=Sound::None;
        if(encoder)EncoderFeedback::handleInputEvent(&event);
        else BuzzerFeedbackThread::handleInputEvent(&event);
        Feedback actual;
        actual.input(code,mode,encoder,23,[](unsigned){return true;});
        const Feedback::Note *expected=nullptr;
        switch(selected){
        case Sound::Click:expected=Feedback::click;break;
        case Sound::Beep:expected=Feedback::beep;break;
        case Sound::Boop:expected=Feedback::boop;break;
        case Sound::Chirp:expected=Feedback::chirp;break;
        case Sound::Combo:expected=Feedback::combo;break;
        case Sound::None:break;
        }
        assert(expected==actual.notes);++cases;
    }
    std::printf("BuzzerFeedbackThread %u event/mode/encoder comparisons\n",cases);
}
static void statusAndWatchdog(){
    mt::stock::StatusLedRadio actual;
    stock::StatusLEDModule donor;
    for(uint32_t start:{0u,UINT32_MAX-25}){
        stock::now=start;
        donor.handleLoRaRx(0);actual.handleLoRaRx(start);
        for(unsigned elapsed:{0u,1u,49u,99u,100u,101u}){
            stock::now=start+elapsed;donor.poll();assert(actual.runOnce(stock::now)==stock::led);
        }
    }
    struct Pins {
        std::vector<unsigned> events;
        bool configure(){events.push_back(120);events.push_back(111);return true;}
        void delayMs(unsigned ms){assert(ms==1);events.push_back(3);}
        bool done(bool level){events.push_back(level);return true;}
    } pins;
    assert(stock::WatchdogThread::setup() && mt::stock::watchdog::setup(pins) && pins.events==stock::edges);
    stock::WatchdogThread::feedDog();
    assert(mt::stock::watchdog::feedDog(pins) && pins.events==stock::edges);
    std::puts("StatusLEDModule 12 RX flash comparisons");
    std::puts("WatchdogThread 2 setup/feed traces");
}
static void power(){
    using namespace mt::stock::powerstress;
    using namespace stock;
    unsigned cases=0;
    const Opcode supported[]={
#include "opcodes.inc"
    };
    for(auto op:supported){
        meshtastic_PowerStressMessage q{};q.cmd=op;q.num_seconds=.25f;
        Module port;stock::PowerStressModule ref;
        called=Action::None;now=1000;
        ref.handleReceivedProtobuf({},&q);
        if(q.cmd!=meshtastic_PowerStressMessage_Opcode_PRINT_INFO){
            assert(ref.currentMessage.cmd==q.cmd);
            called=Action::Mark;
            ref.runOnce();
        }
        assert(port.receive(1,q)==0);
        auto step=port.poll(1000);
        assert(step.action==called);
        if(step.action!=Action::Info){assert(step.remainingMs==250 && step.started);ref.runOnce();assert(!ref.isRunningCommand);}
        ++cases;
    }
    assert(cases==15);
    std::printf("PowerStressModule %u opt-in action comparisons\n",cases);
}
namespace gpio {
#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
struct {struct {bool enabled=true,allow_undefined_pin_access=false;}remote_hardware;} moduleConfig;
constexpr unsigned INPUT_PULLUP=0,OUTPUT=1;
uint32_t now;
uint32_t millis(){return now;}
struct Throttle{static bool isWithinTimespanMs(uint32_t last,uint32_t interval){return now-last<interval;}};
uint64_t pins;
void pinMode(unsigned,unsigned){}
void digitalWrite(unsigned pin,bool state){const uint64_t bit=UINT64_C(1)<<pin;pins=state?(pins|bit):(pins&~bit);}
bool digitalRead(unsigned pin){return (pins>>pin)&1;}
struct {void sendToMesh(meshtastic_MeshPacket*){}} mesh,*service=&mesh;
struct RemoteHardwareModule {
    uint64_t availablePins=0,watchGpios=0,previousWatch=0;
    uint32_t lastWatchMsec=0;bool enabled=false,emitted=false;
    meshtastic_MeshPacket packet{},*myReply=nullptr;
    meshtastic_HardwareMessage result{};
    meshtastic_MeshPacket *allocDataProtobuf(const meshtastic_HardwareMessage &m){result=m;emitted=true;return &packet;}
    void setReplyTo(meshtastic_MeshPacket*,const meshtastic_MeshPacket&){}
    int32_t disable(){return -1;}
    void setInterval(unsigned){}
    bool handleReceivedProtobuf(const meshtastic_MeshPacket&,meshtastic_HardwareMessage*);
    int32_t runOnce();
};
#include "gpio.inc"
}
static void gpioCases(){
    using namespace gpio;
    const uint64_t top=UINT64_C(1)<<63;
    unsigned cases=0;
    for(uint64_t mask:{UINT64_C(0),UINT64_C(0x24),top}) {
        RemoteHardwareModule ref;
        ref.availablePins=mask;
        meshtastic_HardwareMessage q{},reply{};
        uint64_t ztPins=0,watched=0;uint32_t last=0;uint64_t previous=0;bool first=true;
        pins=0;
        auto apply=[&](){
            ref.handleReceivedProtobuf({},&q);
            return mt::stock::RemoteHardwareModule::handleReceivedProtobuf(q,
                [&](uint64_t m,uint64_t v){ztPins=(ztPins&~m)|(m&v);return 1;},
                [&](uint64_t m,uint64_t &v){v=ztPins&m;return true;},
                [&](uint64_t m){watched=m;return 1;},
                [&](const meshtastic_HardwareMessage &r){reply=r;return 1;},-1,0);
        };
        q.gpio_mask=mask;q.gpio_value=UINT64_MAX;q.type=meshtastic_HardwareMessage_Type_WRITE_GPIOS;
        assert(apply()==1&&ztPins==pins);++cases;
        q.type=meshtastic_HardwareMessage_Type_READ_GPIOS;
        assert(apply()==1&&ref.emitted&&reply.gpio_value==ref.result.gpio_value&&reply.gpio_mask==ref.result.gpio_mask);++cases;
        q.type=meshtastic_HardwareMessage_Type_WATCH_GPIOS;assert(apply()==1&&watched==ref.watchGpios);++cases;
        for(uint32_t t:{29999u,30000u,59999u,60000u,90000u,UINT32_MAX,5u}){
            now=t;if(t==90000u)pins=ztPins=0;
            ref.emitted=false;ref.runOnce();
            bool sent=mt::stock::RemoteHardwareModule::runOnce(true,watched,t,ztPins,last,previous,first,reply);
            assert(sent==ref.emitted);
            if(sent)assert(reply.type==ref.result.type&&reply.gpio_value==ref.result.gpio_value&&reply.gpio_mask==ref.result.gpio_mask);
            ++cases;
        }
    }
    std::printf("RemoteHardwareModule %u command/watch comparisons\n",cases);
}
int main(){buzzer();statusAndWatchdog();power();gpioCases();}
