// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SatellitePages.h"
#include "SatelliteReplay.h"
#include "../RemoteTelemetryQuery.h"
namespace mt::companion {
struct SatelliteWire {
    static bool encode(const pb_msgdesc_t *msg,const void *in,uint8_t *out,size_t cap,size_t &n){
        auto stream=pb_ostream_from_buffer(out,cap);if(!pb_encode(&stream,msg,in))return false;n=stream.bytes_written;return true;
    }
    static bool position(const PeerPosition &pos,uint8_t *p,size_t &n){
        meshtastic_Position wire{};pos.exportReplay(wire);return encode(meshtastic_Position_fields,&wire,p,32,n);
    }
    static bool device(const PeerDeviceMetrics &d,uint8_t *p,size_t &n){
        meshtastic_DeviceMetrics wire{};d.exportTo(wire);return encode(meshtastic_DeviceMetrics_fields,&wire,p,32,n);
    }
    static int retainFixTime(const uint8_t *previous,size_t oldSize,uint8_t (&next)[32],size_t &size){
        meshtastic_Position current{},old{};
        auto a=pb_istream_from_buffer(next,size),b=pb_istream_from_buffer(previous,oldSize);
        if(!pb_decode(&a,meshtastic_Position_fields,&current))return -EILSEQ;
        if(current.time)return 0;
        if(!pb_decode(&b,meshtastic_Position_fields,&old))return -EILSEQ;
        if(!old.time)return 0;
        current.time=old.time;
        return encode(meshtastic_Position_fields,&current,next,sizeof(next),size)?0:-EILSEQ;
    }
    static bool hot(const NodeCache::Entry &e,SatelliteReplay &live,unsigned kind,uint8_t *out,size_t &n){
        if(kind==SatelliteSample::Position)return e.replayPosition&&position(e.position,out,n);
        if(kind==SatelliteSample::Device)return e.replayDevice&&device(e.deviceMetrics,out,n);
        if(kind==SatelliteSample::Environment){
            size_t count=0;const auto *p=live.environment(e.id(),count);
            if(!e.replayEnvironment||!p||!count||count>233)return false;
            memcpy(out,p,count);n=count;return true;
        }
        if(kind!=SatelliteSample::Status||!e.status.replayable())return false;
        n=strlen(e.status.text());memcpy(out,e.status.text(),n);return true;
    }
    static bool packet(const SatelliteSample &s,uint32_t currentHeard,unsigned hopLimit,meshtastic_MeshPacket &p){
        return s.valid()&&s.size&&compose(s,currentHeard,hopLimit,p);
    }
    static bool livePacket(const NodeCache::Entry &peer,SatelliteReplay &live,SatelliteReplay::Kind kind,
                           unsigned hopLimit,meshtastic_MeshPacket &out){
        SatelliteSample s{};size_t size=0;
        if(!hot(peer,live,kind,s.bytes,size)||!size)return false;
        s.owner.id=peer.id();s.kind=kind;s.channel=live.channel(peer.id(),kind);s.size=size;
        auto observation=live.received(peer.id(),kind);
        s.quarterSnr=observation.restored()?observation.quarterSnr:int16_t(peer.snr)*4;
        s.flags=observation.restored()?observation.flags:uint8_t((peer.heard?8+peer.hops:0)|(peer.viaMqtt?16:0));
        return compose(s,peer.lastHeard(),hopLimit,out);
    }
private:
    static bool compose(const SatelliteSample &s,uint32_t currentHeard,unsigned hopLimit,meshtastic_MeshPacket &p){
        p={};p.from=s.owner.id;p.to=UINT32_MAX;p.channel=s.channel;p.rx_snr=s.quarterSnr/4.f;
        p.via_mqtt=s.flags&16;p.rx_time=currentHeard;p.has_rx_time=p.rx_time>=1577836800u;
        if(s.flags&8){p.hop_start=hopLimit;p.hop_limit=(s.flags&7)<hopLimit?hopLimit-(s.flags&7):0;}
        p.priority=meshtastic_MeshPacket_Priority_BACKGROUND;p.transport_mechanism=meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
        p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;auto &data=p.decoded;
        auto in=pb_istream_from_buffer(s.bytes,s.size);size_t n=0;unsigned discriminator=0;
        if(s.kind==SatelliteSample::Position){
            meshtastic_Position pos{};if(!pb_decode(&in,meshtastic_Position_fields,&pos))return false;
            pos.has_latitude_i=pos.has_longitude_i=true;PeerPosition checked;
            if(!checked.assign(pos)||!position(checked,data.payload.bytes,n)||n!=s.size||memcmp(data.payload.bytes,s.bytes,n))return false;
            discriminator=data.portnum=meshtastic_PortNum_POSITION_APP;
        }else if(s.kind==SatelliteSample::Status){
            if(memchr(s.bytes,0,s.size))return false;
            meshtastic_StatusMessage status{};memcpy(status.status,s.bytes,s.size);
            if(!encode(meshtastic_StatusMessage_fields,&status,data.payload.bytes,sizeof(data.payload.bytes),n))return false;
            discriminator=data.portnum=meshtastic_PortNum_NODE_STATUS_APP;
        }else{
            meshtastic_Telemetry telemetry{};
            if(s.kind==SatelliteSample::Device){
                telemetry.which_variant=meshtastic_Telemetry_device_metrics_tag;
                auto &m=telemetry.variant.device_metrics;PeerDeviceMetrics safe;
                if(!pb_decode(&in,meshtastic_DeviceMetrics_fields,&m)||!safe.assign(m))return false;
                if(!device(safe,data.payload.bytes,n)||n!=s.size||memcmp(data.payload.bytes,s.bytes,n))return false;
            }else if(!RemoteTelemetryQuery::decodeCanonical(s.bytes,s.size,0,telemetry)||
                telemetry.which_variant!=meshtastic_Telemetry_environment_metrics_tag)return false;
            data.portnum=meshtastic_PortNum_TELEMETRY_APP;discriminator=telemetry.which_variant;telemetry.time=currentHeard;
            if(!encode(meshtastic_Telemetry_fields,&telemetry,data.payload.bytes,sizeof(data.payload.bytes),n))return false;
        }
        data.payload.size=n;uint32_t id=(s.owner.id*2654435761u+p.rx_time)*2654435761u+discriminator;p.id=id?id:1;return true;
    }
};
}
