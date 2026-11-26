// SPDX-License-Identifier: GPL-3.0-only
#include "NodeUser.h"
#include <cassert>
#include <initializer_list>

namespace stock282 {
constexpr uint32_t Self=0x11111111;
uint32_t getFrom(const meshtastic_MeshPacket *p){return p->from?p->from:Self;}
bool pb_decode_from_bytes(const uint8_t *p,size_t n,const pb_msgdesc_t *fields,void *dest){
    auto s=pb_istream_from_buffer(p,n);return pb_decode(&s,fields,dest);
}
size_t pb_encode_to_bytes(uint8_t *p,size_t n,const pb_msgdesc_t *fields,const void *src){
    auto s=pb_ostream_from_buffer(p,n);return pb_encode(&s,fields,src)?s.bytes_written:0;
}
bool isBroadcast(uint32_t n){return n==UINT32_MAX;}
bool isFromUs(const meshtastic_MeshPacket *p){return p->from==Self;}
bool isToUs(const meshtastic_MeshPacket *p){return p->to==Self;}
void stampRxTime(meshtastic_MeshPacket *p){p->rx_time=1792660000;}
struct {struct {bool enabled=true,encryption_enabled=true;} mqtt;} moduleConfig;
struct Capture {
    unsigned calls=0;
    void onSend(const meshtastic_MeshPacket &p,const meshtastic_MeshPacket &,unsigned channel){
        assert(p.pki_encrypted&&p.rx_time==1792660000&&channel==0);++calls;
    }
} capture;
auto *mqtt=&capture;
struct Router {
    static constexpr unsigned opaqueUplinkCapacity=8;
    struct {uint32_t from,id;} opaqueUplinkSeen[8]{};
    unsigned opaqueUplinkNext=0;
    void perhapsUplinkOpaquePki(meshtastic_MeshPacket *);
};
#include "identity-oracle.inc"
}
void compareIdentityPolicy(){
    // The production Service RX test drives these same eight inserts, duplicates,
    // rollover, zero-hop, disabled MQTT, ignored IDs and nonlocal RF conditions.
    stock282::Router router;
    meshtastic_MeshPacket radio{};radio.from=42;radio.to=43;
    for(uint32_t i=1;i<=8;++i){radio.id=i;router.perhapsUplinkOpaquePki(&radio);}
    radio.id=1;router.perhapsUplinkOpaquePki(&radio);assert(stock282::capture.calls==8);
    radio.id=9;router.perhapsUplinkOpaquePki(&radio);
    radio.id=1;router.perhapsUplinkOpaquePki(&radio);assert(stock282::capture.calls==10);
    for(unsigned scenario=0;scenario<7;++scenario){
        meshtastic_MeshPacket p{};p.from=42;p.to=43;p.id=20;
        if(scenario==0)p.channel=1;
        if(scenario==1)p.to=UINT32_MAX;
        if(scenario==2)p.from=stock282::Self;
        if(scenario==3)p.to=stock282::Self;
        if(scenario==4)p.via_mqtt=true;
        if(scenario==5)p.id=0;
        stock282::moduleConfig.mqtt.encryption_enabled=scenario!=6;
        router.perhapsUplinkOpaquePki(&p);assert(stock282::capture.calls==10);
    }
    for(auto value:{"","!00000063","!0000002a"})for(bool extended:{false,true}){
        meshtastic_MeshPacket expected{};expected.from=42;
        expected.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
        auto &data=expected.decoded;data.portnum=meshtastic_PortNum_NODEINFO_APP;
        meshtastic_User u{};strcpy(u.id,value);strcpy(u.long_name,"Peer");
        data.payload.size=stock282::pb_encode_to_bytes(data.payload.bytes,sizeof(data.payload.bytes),meshtastic_User_fields,&u);
        if(extended){data.payload.bytes[data.payload.size++]=0x78;data.payload.bytes[data.payload.size++]=1;}
        auto local=data;
        assert(mt::NodeUser::coerceId(local,42)==stock282::coerceNodeInfoUserId(expected));
        assert(local.payload.size==data.payload.size&&!memcmp(local.payload.bytes,data.payload.bytes,data.payload.size));
    }
}
