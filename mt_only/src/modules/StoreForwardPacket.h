// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "StoreForwardArchive.h"
#include "StoreForwardWire.h"
#include "meshtastic/mesh.pb.h"
#include <pb_encode.h>

namespace mt::modules {
// Internal generated packets only. No companion operation can select a sender.
// Stock remote replay carries the archived author in its shared-channel header;
// this is forwarded history, never a signature or authenticated author claim.
class StoreForwardPacket {
  uint8_t data_[239]{};
  size_t size_=0;
  uint32_t from_=0,to_=0,originalId_=0;
  uint8_t channel_=0,hop_=0,start_=0;
  bool viaMqtt_=false;
  static bool node(uint32_t n){return n>1&&n<0xfffffffeu;}
  static bool encode(uint32_t from,uint32_t to,uint8_t channel,uint8_t hop,uint8_t start,
                     bool mqtt,const meshtastic_Data &data,StoreForwardPacket &out){
    if(!node(from)||(!node(to)&&to!=UINT32_MAX)||from==to||channel>=8||hop>start||start>7)return false;
    StoreForwardPacket candidate;
    auto stream=pb_ostream_from_buffer(candidate.data_,sizeof(candidate.data_));
    if(!pb_encode(&stream,meshtastic_Data_fields,&data)||!stream.bytes_written)return false;
    candidate.size_=stream.bytes_written;candidate.from_=from;candidate.to_=to;
    candidate.channel_=channel;candidate.hop_=hop;candidate.start_=start;candidate.viaMqtt_=mqtt;
    out=candidate;return true;
  }
public:
  bool valid()const{return size_!=0;}
  const uint8_t *data()const{return data_;}
  size_t size()const{return size_;}
  uint32_t from()const{return from_;}
  uint32_t to()const{return to_;}
  uint32_t originalId()const{return originalId_;}
  uint8_t channel()const{return channel_;}
  uint8_t flags()const{return uint8_t(hop_|(start_<<5)|(viaMqtt_?0x10:0));}
  static bool control(uint32_t local,uint32_t to,uint8_t channel,uint8_t hop,
                      const meshtastic_StoreAndForward &message,StoreForwardPacket &out){
    // Inspect through the safe codec before any enum value access.
    uint8_t wire[store_forward::StoreForwardWire::MaxEncodedBytes];size_t n=0;
    if(!store_forward::StoreForwardWire::encode(message,wire,sizeof(wire),&n)||n>233)return false;
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_STORE_FORWARD_APP;
    data.has_bitfield=true;data.payload.size=pb_size_t(n);memcpy(data.payload.bytes,wire,n);
    return encode(local,to,channel,hop,hop,false,data,out);
  }
  static bool error(uint32_t local,uint32_t to,uint8_t channel,uint8_t hop,bool busy,StoreForwardPacket &out){
    const char *text=busy?"S&F - Busy. Try again shortly.":"S&F not permitted on the public channel.";
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    data.has_bitfield=true;data.payload.size=pb_size_t(strlen(text));memcpy(data.payload.bytes,text,data.payload.size);
    return encode(local,to,channel,hop,hop,false,data,out);
  }
  static bool replay(uint32_t requester,const StoreForwardArchive::Record &record,StoreForwardPacket &out){
    if(!record.id||record.from==requester||record.payloadSize>StoreForwardArchive::MaxPayload||
       (record.to!=UINT32_MAX&&record.to!=requester))return false;
    meshtastic_StoreAndForward message=meshtastic_StoreAndForward_init_zero;
    message.rr=record.to==UINT32_MAX?meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST:
                                  meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_DIRECT;
    message.which_variant=meshtastic_StoreAndForward_text_tag;
    message.original_id=record.id;message.variant.text.size=record.payloadSize;
    memcpy(message.variant.text.bytes,record.payload,record.payloadSize);
    uint8_t wire[store_forward::StoreForwardWire::MaxEncodedBytes];size_t n=0;
    if(!store_forward::StoreForwardWire::encode(message,wire,sizeof(wire),&n)||n>233)return false;
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_STORE_FORWARD_APP;
    data.has_bitfield=true;data.payload.size=pb_size_t(n);memcpy(data.payload.bytes,wire,n);
    data.reply_id=record.replyId;data.emoji=record.emoji?1:0;
    StoreForwardPacket candidate;
    if(!encode(record.from,requester,record.channel,record.hopLimit,record.hopStart,record.viaMqtt,data,candidate))return false;
    candidate.originalId_=record.id;out=candidate;return true;
  }
};
}
