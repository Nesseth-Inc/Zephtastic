// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PublicNodeWire.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

namespace mt::network::browser {
enum class Operation { Invalid, Denied, Start, Text, Heartbeat, Disconnect };
// Nanopb writes enum representations at their generated width, even for an
// unknown value. Compare the bytes before evaluating an incoming C++ enum.
template<class E> inline bool fieldIs(const E &field,E allowed) {
    return memcmp(&field,&allowed,sizeof(E))==0;
}
inline bool textMessage(const meshtastic_Data &d) {
    if (!fieldIs(d.portnum,meshtastic_PortNum_TEXT_MESSAGE_APP) || !d.payload.size) return false;
    const auto *p=d.payload.bytes;
    if (d.payload.size>=3 && !memcmp(p,"zt_",3)) return false;
    // The console reserves its control prefix. Accept printable UTF-8 and ordinary text whitespace.
    for(size_t i=0;i<d.payload.size;) {
        unsigned c=p[i++];
        if(c<0x80) {if ((c<0x20 && c!='\n' && c!='\r' && c!='\t') || c==0x7f) return false;continue;}
        unsigned n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!n||i+n>d.payload.size)return false;
        unsigned next=p[i];
        if((c==0xe0&&next<0xa0)||(c==0xed&&next>=0xa0)||(c==0xf0&&next<0x90)||(c==0xf4&&next>=0x90))return false;
        while(n--)if((p[i++]&0xc0)!=0x80)return false;
    }
    return true;
}
inline bool encode(const pb_msgdesc_t *fields,const void *value,uint8_t *out,size_t cap,size_t &size) {
    auto stream=pb_ostream_from_buffer(out,cap);
    if(!pb_encode(&stream,fields,value))return false;
    size=stream.bytes_written;return true;
}
struct Wire {
    meshtastic_ToRadio in{}, admitted{};
    meshtastic_FromRadio outgoing{}, publicCopy{};
    uint8_t compared[512]{};
    // One top-level operation. Re-encode the allowed fields before handing any body to Companion.
    Operation input(const uint8_t *bytes,size_t n,uint8_t *out,size_t capacity,size_t &size) {
        size=0;in={};admitted={};
        if(!bytes || !n || n>512)return Operation::Invalid;
        auto scan=pb_istream_from_buffer(bytes,n);
        pb_wire_type_t type;uint32_t tag;bool eof=false;
        if(!pb_decode_tag(&scan,&type,&tag,&eof)||!pb_skip_field(&scan,type)||scan.bytes_left)return Operation::Invalid;
        auto stream=pb_istream_from_buffer(bytes,n);
        if(!pb_decode(&stream,meshtastic_ToRadio_fields,&in)||in.which_payload_variant!=tag)return Operation::Invalid;
        admitted.which_payload_variant=in.which_payload_variant;
        Operation kind=Operation::Denied;
        switch(tag) {
          case meshtastic_ToRadio_want_config_id_tag:
            if(!in.want_config_id)return Operation::Invalid;
            admitted.want_config_id=in.want_config_id;kind=Operation::Start;break;
          case meshtastic_ToRadio_disconnect_tag:
            if(!in.disconnect)return Operation::Invalid;
            admitted.disconnect=true;kind=Operation::Disconnect;break;
          case meshtastic_ToRadio_heartbeat_tag:
            if(in.heartbeat.nonce==1)return Operation::Denied; // Radio discovery is outside browser messaging.
            admitted.heartbeat.nonce=in.heartbeat.nonce;kind=Operation::Heartbeat;break;
          case meshtastic_ToRadio_packet_tag:{
            const auto &p=in.packet;auto &q=admitted.packet;
            if(p.which_payload_variant!=meshtastic_MeshPacket_decoded_tag||!p.id||!p.to||p.channel>7||p.hop_limit>7||!textMessage(p.decoded))return Operation::Denied;
            if(!fieldIs(p.priority,meshtastic_MeshPacket_Priority_UNSET) && !fieldIs(p.priority,meshtastic_MeshPacket_Priority_DEFAULT) &&
               !fieldIs(p.priority,meshtastic_MeshPacket_Priority_RELIABLE))return Operation::Denied;
            q.which_payload_variant=meshtastic_MeshPacket_decoded_tag;q.to=p.to;q.id=p.id;q.channel=p.channel;
            q.want_ack=p.want_ack;q.hop_limit=p.hop_limit;q.priority=p.priority;
            q.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;q.decoded.payload=p.decoded.payload;
            kind=Operation::Text;break;
          }
          default:return Operation::Denied;
        }
        size_t normalized=0;
        if(!encode(meshtastic_ToRadio_fields,&in,compared,sizeof(compared),normalized)||
           !encode(meshtastic_ToRadio_fields,&admitted,out,capacity,size))return Operation::Invalid;
        return normalized==size&&!memcmp(out,compared,size)?kind:Operation::Denied;
    }
    // Only public discovery, text, delivery status, and the synchronization marker reach HTTP.
    // Build every record from its explicit public fields. Config and ModuleConfig can contain secrets.
    bool output(const uint8_t *bytes,size_t n,uint8_t *out,size_t capacity,size_t &size) {
        outgoing={};publicCopy={};size=0;
        if(!bytes||!n||n>512)return false;
        auto stream=pb_istream_from_buffer(bytes,n);
        if(node::isNodeRecord(stream)) {
            return pb_decode(&stream,&node::Envelope_msg,&publicCopy)&&
                   encode(&node::Envelope_msg,&publicCopy,out,capacity,size);
        }
        if(!pb_decode(&stream,meshtastic_FromRadio_fields,&outgoing))return false;
        const auto &v=outgoing;auto &p=publicCopy;
        p.id=v.id;p.which_payload_variant=v.which_payload_variant;
        switch(v.which_payload_variant) {
          case meshtastic_FromRadio_my_info_tag:
            p.my_info.my_node_num=v.my_info.my_node_num;p.my_info.reboot_count=v.my_info.reboot_count;
            p.my_info.min_app_version=v.my_info.min_app_version;break;
          case meshtastic_FromRadio_config_complete_id_tag:p.config_complete_id=v.config_complete_id;break;
          case meshtastic_FromRadio_channel_tag:
            if(v.channel.index<0||v.channel.index>7 ||
               (!fieldIs(v.channel.role,meshtastic_Channel_Role_PRIMARY)&&!fieldIs(v.channel.role,meshtastic_Channel_Role_SECONDARY)&&
                !fieldIs(v.channel.role,meshtastic_Channel_Role_DISABLED)))return false;
            p.channel.index=v.channel.index;p.channel.role=v.channel.role;p.channel.has_settings=v.channel.has_settings;
            if(v.channel.has_settings)memcpy(p.channel.settings.name,v.channel.settings.name,sizeof(p.channel.settings.name));
            break;
          case meshtastic_FromRadio_clientNotification_tag:{
            const auto &note=v.clientNotification;
            if(!note.has_reply_id||!note.reply_id||!fieldIs(note.level,meshtastic_LogRecord_Level_ERROR) ||
               note.which_payload_variant)return false;
            auto &safe=p.clientNotification;
            safe.has_reply_id=true;safe.reply_id=note.reply_id;safe.level=meshtastic_LogRecord_Level_ERROR;
            const char *message=!strncmp(note.message,"RF TX is disarmed",17) ||
                !strcmp(note.message,"Radio is unavailable or TX is disarmed")?"Radio is unavailable or TX is disarmed":
                !strcmp(note.message,"Local message queue is full")?"Local message queue is full":"Radio rejected this message";
            strcpy(safe.message,message);break;
          }
          case meshtastic_FromRadio_queueStatus_tag:p.queueStatus=v.queueStatus;break;
          case meshtastic_FromRadio_packet_tag:{
            const auto &src=v.packet;auto &dest=p.packet;
            if(src.which_payload_variant!=meshtastic_MeshPacket_decoded_tag)return false;
            if(!textMessage(src.decoded)) {
                if(!fieldIs(src.decoded.portnum,meshtastic_PortNum_ROUTING_APP)||!src.decoded.request_id)return false;
                meshtastic_Routing routing{};
                auto data=pb_istream_from_buffer(src.decoded.payload.bytes,src.decoded.payload.size);
                if(!pb_decode(&data,meshtastic_Routing_fields,&routing)||
                   routing.which_variant!=meshtastic_Routing_error_reason_tag)return false;
                size_t len;
                if(!encode(meshtastic_Routing_fields,&routing,dest.decoded.payload.bytes,sizeof(dest.decoded.payload.bytes),len))return false;
                dest.decoded.payload.size=len;
            }else dest.decoded.payload=src.decoded.payload;
            dest.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
            dest.from=src.from;dest.to=src.to;dest.id=src.id;dest.channel=src.channel;
            dest.has_rx_time=src.has_rx_time;dest.rx_time=src.rx_time;
            dest.decoded.portnum=src.decoded.portnum;dest.decoded.request_id=src.decoded.request_id;
            dest.decoded.reply_id=src.decoded.reply_id;break;
          }
          default:return false;
        }
        return encode(meshtastic_FromRadio_fields,&p,out,capacity,size);
    }
};
}
