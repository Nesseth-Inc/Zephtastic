// SPDX-License-Identifier: GPL-3.0-only
// Independent clock, pin, archive and packet sinks for pinned upstream methods.
#pragma once
#include <meshtastic/mesh.pb.h>
#include <meshtastic/remote_hardware.pb.h>
#include <meshtastic/storeforward.pb.h>
#include <meshtastic/telemetry.pb.h>
#include <stdint.h>
#include <map>
#include <cmath>
#include <cstdlib>

namespace donor {
#define ARCH_PORTDUINO 1
using NodeNum = uint32_t;
constexpr double PI=3.14159265358979323846, DEG_CONVERT=180/PI;
struct GeoCoord {static float latLongToMeter(double,double,double,double);};
constexpr NodeNum NODENUM_BROADCAST=UINT32_MAX;
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
using ChannelIndex = uint8_t;
constexpr unsigned BITFIELD_OK_TO_MQTT_MASK = 1;
struct { struct {bool encryption_enabled;} mqtt; struct {bool enabled;} remote_hardware, store_forward; } moduleConfig;
struct Channels {
    struct Channel { struct {bool uplink_enabled;} settings; } slots[8];
    Channel &getByIndex(unsigned i) {return slots[i];}
    const char *getGlobalId(unsigned) {return "local";}
} channels;
bool isMqttServerAddressPrivate, isConfiguredForDefaultServer;
bool isFromUs(const meshtastic_MeshPacket *mp) {return mp->from == 42;}
// These admission vectors use text, range and sensor ports; any NodeInfo
// normalization would mean the donor applied its branch to a different port.
struct {
    meshtastic_MeshPacket *allocCopy(const meshtastic_MeshPacket &, unsigned) {
        std::abort();
    }
} packetPool;
bool coerceNodeInfoUserId(meshtastic_MeshPacket &) { std::abort(); }
const meshtastic_MeshPacket *published;
struct MQTT {void onSend(const meshtastic_MeshPacket &, const meshtastic_MeshPacket &, ChannelIndex);};

uint32_t now, epoch;
uint32_t millis(){return now;}
uint32_t getTime(){return epoch;}
struct {bool allowed=false; bool isTxAllowedChannelUtil(bool polite){return polite && allowed;}} air;
auto *airTime = &air;
struct StoreForwardModule {
    uint32_t last_time=0, historyReturnMax=0, busyTo=0, packetTimeMax=5000;
    std::map<NodeNum,uint32_t> lastRequest;
    struct Record {uint32_t time,from,to;} packetHistory[3]{};
    bool is_server=true, attempted=false, available=false;
    uint32_t requestCount=0,lastHeartbeat=0,heartbeatInterval=900;
    uint32_t records=0, packetHistoryTotalCount=0, requests=0, requests_history=0, historyReturnWindow=0;
    bool busy=false, heartbeat=false;
    meshtastic_StoreAndForward reply{};
    uint32_t getNumAvailablePackets(NodeNum,uint32_t);
    bool sendPayload(NodeNum,uint32_t){attempted=true;if(available)++requestCount;return available;}
    int32_t runOnce();
    int32_t disable(){return -1;}
    void sendMessage(uint32_t,const meshtastic_StoreAndForward &sf){reply=sf;}
    void setIntervalFromNow(uint32_t){}
    void historySend(uint32_t, uint32_t);
    void statsSend(uint32_t);
    void observe(const meshtastic_StoreAndForward *p) {
        const bool is_client=true;
        switch (p->rr) {
#include "store_client.inc"
        default: break;
        }
    }
};
StoreForwardModule *storeForwardModule;

constexpr unsigned NUM_GPIOS=64, OUTPUT=1, WATCH_INTERVAL_MSEC=30000;
uint64_t pins;
void pinModes(uint64_t, unsigned, uint64_t){}
bool pinAccessAllowed(uint64_t mask, unsigned pin){return mask & (UINT64_C(1)<<pin);}
uint64_t digitalReads(uint64_t mask, uint64_t allowed){return pins & mask & allowed;}
void digitalWrite(unsigned pin, bool value){
    const uint64_t mask=UINT64_C(1)<<pin;
    pins = value ? pins | mask : pins & ~mask;
}
meshtastic_MeshPacket packet{};
struct {void sendToMesh(meshtastic_MeshPacket*){}} serviceInstance, *service=&serviceInstance;
struct Throttle {static bool isWithinTimespanMs(uint32_t then,uint32_t interval){return now-then<interval;}};
struct RemoteHardwareModule {
    uint64_t availablePins=UINT64_MAX,watchGpios=0,previousWatch=0;
    uint32_t lastWatchMsec=0;
    bool enabled=false, emitted=false;
    meshtastic_MeshPacket *myReply=nullptr;
    meshtastic_HardwareMessage result{};
    meshtastic_MeshPacket *allocDataProtobuf(const meshtastic_HardwareMessage &m){result=m; emitted=true;return &packet;}
    void setReplyTo(meshtastic_MeshPacket *, const meshtastic_MeshPacket &){}
    void setInterval(int32_t){}
    int32_t disable(){return -1;}
    bool handleReceivedProtobuf(const meshtastic_MeshPacket &, meshtastic_HardwareMessage *);
    int32_t runOnce();
};
struct TelemetrySensor {
    bool active, ok;
    unsigned kind, calls=0;
    bool hasSensor() const{return active;}
    bool getMetrics(meshtastic_Telemetry *t){
        ++calls;
        if(ok && kind==0){auto &m=t->variant.air_quality_metrics; m.has_co2=true;m.co2=432;}
        if(ok && kind==1){auto &m=t->variant.health_metrics; m.has_heart_bpm=true;m.heart_bpm=72;}
        if(ok && kind==2){auto &m=t->variant.health_metrics; m.has_temperature=true;m.temperature=37.5f;}
        return ok;
    }
};
struct AirQualityTelemetryModule {TelemetrySensor *sensors[1]; bool getAirQualityTelemetry(meshtastic_Telemetry *);};
TelemetrySensor max30102Sensor{},mlx90614Sensor{};
struct HealthTelemetryModule {bool getHealthTelemetry(meshtastic_Telemetry *);};
#include "oracle.inc"
#undef LOG_INFO
#undef LOG_DEBUG
#undef LOG_ERROR
}
