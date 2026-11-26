// SPDX-License-Identifier: GPL-3.0-only
// Host-only dependencies for original donor methods. Physical operations just record an effect.
#pragma once
#include <cassert>
#include <pb_decode.h>
#include <pb_encode.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "meshtastic/mesh.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/module_config.pb.h"

namespace oracle {
enum class ProcessMessage { CONTINUE };
struct {
    meshtastic_ModuleConfig_SerialConfig serial{};
    meshtastic_ModuleConfig_CannedMessageConfig canned_message{};
    meshtastic_ModuleConfig_ExternalNotificationConfig external_notification{};
    meshtastic_ModuleConfig_DetectionSensorConfig detection_sensor{};
    meshtastic_ModuleConfig_AmbientLightingConfig ambient_lighting{};
} moduleConfig;
struct { meshtastic_Config_DeviceConfig device{}; } config;
bool isFromUs(const meshtastic_MeshPacket *p) { return p->from == 1; }
bool isToUs(const meshtastic_MeshPacket *p) { return p->to == 1; }
bool isBroadcast(uint32_t n) { return n == UINT32_MAX; }
auto getFrom(const meshtastic_MeshPacket *p) { return p->from; }
bool muted;
bool isMutedForPacket(const meshtastic_MeshPacket &) { return muted; }
bool level;
bool digitalRead(unsigned) { return level; }
uint32_t now;
uint32_t millis() { return now; }

struct Print {
    std::string out;
    void write(const uint8_t *data, size_t size) { out.append(reinterpret_cast<const char *>(data), size); }
    template <typename... Args> void printf(const char *fmt, Args... args) {
        char buf[280];
        const int size = std::snprintf(buf, sizeof(buf), fmt, args...);
        assert(size >= 0 && unsigned(size) < sizeof(buf));
        out.append(buf, size);
    }
    void println() { out += "\r\n"; }
} uart;
auto serialPrint = &uart;
using NodeNum=uint32_t;
using ChannelIndex=uint8_t;
constexpr int RX_SRC_LOCAL=0;
struct meshtastic_NodeInfoLite { const char *short_name, *long_name; uint32_t num=0; struct { unsigned size=0; } public_key{}; };
struct Nodes {
    meshtastic_NodeInfoLite *peer{};
    size_t getNumMeshNodes()const{return peer?1:0;}
    meshtastic_NodeInfoLite *getMeshNodeByIndex(size_t i){return i?nullptr:peer;}
    NodeNum getNodeNum()const{return 1;}
    auto getMeshNode(uint32_t) { return peer; }
} nodes;
auto nodeDB = &nodes;
bool nodeInfoLiteHasUser(const meshtastic_NodeInfoLite *n) { return n != nullptr; }
struct SerialModuleRadio {
    uint32_t lastRxID{};
    meshtastic_PortNum ourPortNum = meshtastic_PortNum_SERIAL_APP;
    char outbuf[150]{};
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp);
};
#define HAS_GPS 1
bool pb_decode_from_bytes(const uint8_t *bytes, size_t n, const pb_msgdesc_t *fields, void *out)
{
    auto in = pb_istream_from_buffer(bytes, n);
    return pb_decode(&in, fields, out);
}
struct Waypoint { int32_t lat{}, lon{}, alt{}; std::string name; bool caltopo{}; };
Waypoint waypoint;
void printWPL(char *out, size_t n, const meshtastic_Position &p, const char *name, bool caltopo)
{
    waypoint = {p.latitude_i, p.longitude_i, p.altitude, name, caltopo};
    std::snprintf(out, n, "waypoint");
}

using String = std::string;
struct { char messages[201]{}; } cannedMessageModuleConfig;
#define CANNED_MESSAGE_MODULE_MESSAGE_MAX_COUNT 50
struct CannedMessageModule {
    bool osk_found = false;
    void *screen = nullptr;
    char messageBuffer[201]{};
    char *messages[50]{};
    int messagesCount{},currentMessageIndex{};
    int getNextIndex(); int getPrevIndex();
    NodeNum lastDest{},lastSentNode{},incoming{};
    unsigned lastChannel{}; bool lastDestSet=false,waitingForAck=false;uint32_t lastRequestId{};
    meshtastic_MeshPacket packet{};
    meshtastic_MeshPacket *allocDataPacket(){return &packet;}
    void sendText(NodeNum,ChannelIndex,const char *,bool);
    int splitConfiguredMessages();
    void handleSetCannedMessageModuleMessages(const char *);
    unsigned saves{};
    bool saveProtoForModule() { ++saves; return true; }
};

struct InputEvent {};
namespace rtttl {
inline bool playing = false;
inline bool isPlaying(){return playing;}
inline void stop(){playing=false;}
}
uint32_t externalTurnedOn[3]{};
struct Throttle {
    static bool isWithinTimespanMs(uint32_t last, uint32_t interval){return uint32_t(now-last)<interval;}
    static bool hasElapsed(uint32_t last, uint32_t interval){return !isWithinTimespanMs(last,interval);}
    static bool deadlinePassed(uint32_t deadline){return uint32_t(now-deadline)<0x80000000u;}
};
#define EXT_NOTIFICATION_MODULE_OUTPUT_MS 1000
#define MESHTASTIC_EXCLUDE_RTTTL 1
struct ExternalNotificationModule {
    bool isSilenced{}, buzzerShouldAlert{}, canBuzzValue{}, isNagging{};
    bool led{}, vibra{}, buzz{};
    uint32_t nagCycleCutoff{};
    bool canBuzz() const { return canBuzzValue; }
    void setExternalState(int index, bool on) { (index==0?led:index==1?vibra:buzz)=on;externalTurnedOn[index]=now; }
    bool getExternal(int index) { return index==0?led:index==1?vibra:buzz; }
    void stopNow();
    int handleInputEvent(const InputEvent *);
    int32_t runOnce();
    void triggerVibraOutput() { vibra = true; }
    void triggerBuzzerOutput() { buzz = true; }
    void setIntervalFromNow(int) {}
    void armNagCycle();
    ProcessMessage handleReceived(const meshtastic_MeshPacket &);
};
#define ASCII_BELL 7
#define LOG_INFO(...)
#define LOG_TRACE(...)
#define LOG_DEBUG(...)
#define LOG_ERROR(...)
struct DetectionService {
    meshtastic_MeshPacket *sent{};
    void sendToMesh(meshtastic_MeshPacket *p) { sent = p; }
    void sendToMesh(meshtastic_MeshPacket *p,int,bool) {sent=p;}
} sink;
auto service = &sink;
struct { bool isDefaultChannel(int) { return false; } } channels;
struct { void release(meshtastic_MeshPacket *) { std::abort(); } } packetPool;
void pinMode(int, int);
#define LOG_WARN(...)
#define INPUT_PULLUP 2
#define INPUT 0
#define GPIO_POLLING_INTERVAL 100
#define DELAYED_INTERVAL 1000
constexpr uint32_t default_telemetry_broadcast_interval_secs=1800;
struct Default {
    static uint32_t getConfiguredOrDefaultMs(uint32_t secs,uint32_t fallback=45){return 1000*(secs?secs:fallback);}
};
struct DetectionSensorModule {
    bool firstTime=false,wasDetected=false;
    int32_t runOnce();
    int32_t disable(){return INT32_MAX;}
    int32_t setStartDelay(){return 0;}
    uint32_t lastSentToMesh{};
    meshtastic_MeshPacket packet{};
    bool hasDetectionEvent();
    void sendDetectionMessage();
    void sendCurrentStateMessage(bool);
    auto allocDataPacket() { return &packet; }
};
auto configuredTriggerType() { return moduleConfig.detection_sensor.detection_trigger_type; }

#include "upstream.inc"

namespace concurrency {
class OSThread {
public:
    explicit OSThread(const char *) {}
    virtual int32_t runOnce() = 0;
    static int32_t disable() { return INT32_MAX; }
};
}
struct ScanI2C { enum class DeviceType { NONE }; };
struct Observer {};
Observer notifyDeepSleep;
template <typename T, typename Arg> struct CallbackObserver {
    CallbackObserver(T *, int (T::*)(Arg)) {}
    void observe(Observer *) {}
};
int rgb[3]{};
void pinMode(int, int) {}
void analogWrite(int pin, int value) { rgb[pin] = value; }
#define HAS_RGB_LED 1
#define OUTPUT 1
#define RGBLED_RED 0
#define RGBLED_GREEN 1
#define RGBLED_BLUE 2
#include "AmbientLightingThread.h"
struct LightProbe : AmbientLightingThread {
    LightProbe() : AmbientLightingThread(ScanI2C::DeviceType::NONE) {}
    using AmbientLightingThread::runOnce;
};
} // namespace oracle
