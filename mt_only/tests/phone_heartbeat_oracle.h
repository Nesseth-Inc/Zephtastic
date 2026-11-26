// SPDX-License-Identifier: GPL-3.0-only
// Donor implementation against bounded stubs for its Arduino globals.
#pragma once
namespace phoneStock {
using NodeNum=uint32_t;
using ErrorCode=int;
constexpr NodeNum NODENUM_BROADCAST=UINT32_MAX;
constexpr int ERRNO_OK=0, ERRNO_SHOULD_RELEASE=1;
constexpr uint32_t default_node_info_broadcast_secs=3600;
static uint32_t ticks=0;
struct Time {static uint32_t getMillis(){return ticks;}};
struct Throttle{static bool isWithinTimespanMs(uint32_t,uint32_t);};
#include "nodeinfo-reference/Throttle.inc"
struct History {
    uint32_t last=0;
    uint32_t getLastSentToMeshMillis(int)const{return last;}
    void setLastSentToMesh(int){last=ticks?ticks:1;}
} history;
static History *transmitHistory=&history;
static meshtastic_User owner{};
static struct {meshtastic_Config_DeviceConfig device;} config;
static struct {unsigned getNumOnline()const{return 1;}} status;
static auto *nodeStatus=&status;
static struct {std::string getNodeId()const{return "!12345678";}} db;
static auto *nodeDB=&db;
static struct {bool allowed=true;bool isTxAllowedChannelUtil(bool){return allowed;}} air;
static auto *airTime=&air;
namespace Default {
static uint32_t getConfiguredOrDefaultMsScaled(unsigned,unsigned seconds,unsigned){return seconds*1000;}
static uint32_t getConfiguredOrDefaultMs(unsigned value,unsigned fallback){return (value?value:fallback)*1000;}
}
static struct Transport {
    meshtastic_MeshPacket packet{};
    unsigned accepted=0;int status=0;
    void cancelSending(uint32_t){}
    int sendToMesh(meshtastic_MeshPacket *p){if(!status){++accepted;packet=*p;}return status;}
} transport;
static auto *service=&transport;
struct NodeInfoModule {
    uint32_t prevPacketId=0,interval=0;bool shorterTimeout=false,deferHistoryStamp=false;
    bool suppressReplyForCurrentRequest=false,ignoreRequest=false;
    const meshtastic_MeshPacket *currentRequest=nullptr;
    meshtastic_MeshPacket scratch{};
    bool isFromUs(const meshtastic_MeshPacket *)const{return false;}
    meshtastic_MeshPacket *allocDataProtobuf(const meshtastic_User&){scratch={};scratch.id=42;return &scratch;}
    void setIntervalFromNow(uint32_t v){interval=v;}
    bool sendOurNodeInfo(NodeNum,bool,uint8_t,bool);
    meshtastic_MeshPacket *allocReply();
} module;
static auto *nodeInfoModule=&module;
#define DEBUG_HEAP_BEFORE ((void)0)
#define DEBUG_HEAP_AFTER(label,p) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#include "nodeinfo-reference/SendNodeInfo.inc"
#include "nodeinfo-reference/AllocNodeInfo.inc"
static bool receive(const meshtastic_ToRadio &toRadioScratch) {
    bool heartbeatReceived=false;
    switch(toRadioScratch.which_payload_variant){
#include "nodeinfo-reference/PhoneHeartbeat.inc"
    default:break;
    }
    return heartbeatReceived;
}
#undef DEBUG_HEAP_BEFORE
#undef DEBUG_HEAP_AFTER
#undef LOG_TRACE
#undef LOG_INFO
#undef LOG_DEBUG
#undef LOG_WARN
}
