// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <meshtastic/mesh.pb.h>
#include <meshtastic/remote_hardware.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

namespace mt::modules {
struct GpioModuleWire {
  template<class E>static uint32_t number(const E &value) {
    using U=__underlying_type(E);U raw{};memcpy(&raw,&value,sizeof(raw));return uint32_t(raw);
  }
  static bool hardware(const uint8_t *payload,size_t size,meshtastic_HardwareMessage &out) {
    out={};
    if(!payload||!size||size>meshtastic_HardwareMessage_size)return false;
    auto input=pb_istream_from_buffer(payload,size);
    if(!pb_decode(&input,meshtastic_HardwareMessage_fields,&out)||input.bytes_left)return false;
    uint8_t canonical[meshtastic_HardwareMessage_size]{};
    auto encoded=pb_ostream_from_buffer(canonical,sizeof(canonical));
    return pb_encode(&encoded,meshtastic_HardwareMessage_fields,&out)&&
      encoded.bytes_written==size&&!memcmp(canonical,payload,size);
  }
  static bool command(const uint8_t *payload,size_t size) {
    meshtastic_HardwareMessage message{};
    if(size>24||!hardware(payload,size,message))return false;
    const auto type=number(message.type);
    return type==meshtastic_HardwareMessage_Type_WRITE_GPIOS||
      type==meshtastic_HardwareMessage_Type_READ_GPIOS||
      type==meshtastic_HardwareMessage_Type_WATCH_GPIOS;
  }
  static bool encodeRequest(const uint8_t *payload,size_t length,bool wantResponse,
                            uint32_t bitfield,uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!command(payload,length)||bitfield>3||!out)return false;
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
    data.payload.size=pb_size_t(length);memcpy(data.payload.bytes,payload,length);
    data.want_response=wantResponse;data.has_bitfield=true;
    data.bitfield=(bitfield&1u)|(wantResponse?2u:0u);
    auto writer=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&writer,meshtastic_Data_fields,&data))return false;
    size=writer.bytes_written;return true;
  }
  template<class Metadata> static bool requestData(const Metadata &m,uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(m.portnum!=meshtastic_PortNum_REMOTE_HARDWARE_APP||m.request_id||m.reply_id||m.emoji||
       m.bitfield>3||(!m.hasBitfield&&m.bitfield)||
       (m.signatureLength&&m.signatureLength!=64))return false;
    meshtastic_HardwareMessage request{};
    if(m.payloadLength>24||!hardware(m.payload,m.payloadLength,request))return false;
    // Replies remain visible to clients but cannot enter the command backend.
    const auto type=number(request.type);
    if(type!=meshtastic_HardwareMessage_Type_WRITE_GPIOS&&
       type!=meshtastic_HardwareMessage_Type_READ_GPIOS&&
       type!=meshtastic_HardwareMessage_Type_WATCH_GPIOS)return false;
    meshtastic_Data data{};
    data.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
    data.payload.size=m.payloadLength;memcpy(data.payload.bytes,m.payload,m.payloadLength);
    data.want_response=m.wantResponse;data.has_bitfield=m.hasBitfield;data.bitfield=m.bitfield;
    data.xeddsa_signature.size=m.signatureLength;
    if(m.signatureLength)memcpy(data.xeddsa_signature.bytes,m.signature,m.signatureLength);
    auto writer=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&writer,meshtastic_Data_fields,&data))return false;
    size=writer.bytes_written;return true;
  }
  template<class Metadata> static bool receivedData(const Metadata &m,uint8_t *out,
                                                     size_t capacity,size_t &size) {
    size=0;
    if(m.portnum!=meshtastic_PortNum_REMOTE_HARDWARE_APP||m.payloadLength>24)return false;
    meshtastic_HardwareMessage message{};
    if(!hardware(m.payload,m.payloadLength,message))return false;
    const auto type=number(message.type);
    if(type==meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY) {
      if(!m.request_id||m.wantResponse||m.reply_id||m.emoji||!m.hasBitfield||m.bitfield>3||
         (m.signatureLength&&m.signatureLength!=64))return false;
      if(!outgoing(m.portnum,m.payload,m.payloadLength,m.request_id,out,capacity,size))return false;
    } else if(type==meshtastic_HardwareMessage_Type_GPIOS_CHANGED) {
      if(m.request_id||m.wantResponse||m.reply_id||m.emoji||m.bitfield>3||
         (!m.hasBitfield&&m.bitfield)||(m.signatureLength&&m.signatureLength!=64))return false;
      meshtastic_Data data{};data.portnum=meshtastic_PortNum_REMOTE_HARDWARE_APP;
      data.payload.size=m.payloadLength;memcpy(data.payload.bytes,m.payload,m.payloadLength);
      data.has_bitfield=m.hasBitfield;data.bitfield=m.bitfield;
      auto writer=pb_ostream_from_buffer(out,capacity);
      if(!pb_encode(&writer,meshtastic_Data_fields,&data))return false;
      size=writer.bytes_written;
    } else {
      return requestData(m,out,capacity,size);
    }
    if(m.signatureLength) {
      if(m.signatureLength!=64)return false;
      meshtastic_Data data{};auto input=pb_istream_from_buffer(out,size);
      if(!pb_decode(&input,meshtastic_Data_fields,&data)||input.bytes_left)return false;
      data.xeddsa_signature.size=64;memcpy(data.xeddsa_signature.bytes,m.signature,64);
      auto writer=pb_ostream_from_buffer(out,capacity);
      if(!pb_encode(&writer,meshtastic_Data_fields,&data))return false;
      size=writer.bytes_written;
    }
    return true;
  }
  static bool outgoing(unsigned port,const uint8_t *payload,size_t length,uint32_t request,
                       uint8_t *out,size_t capacity,size_t &size) {
    size=0;
    if(!payload||!length||!out)return false;
    if(port==meshtastic_PortNum_REMOTE_HARDWARE_APP) {
      meshtastic_HardwareMessage message{};
      if(!hardware(payload,length,message))return false;
      if(request) {
        if(number(message.type)!=meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY)return false;
      } else if(number(message.type)!=meshtastic_HardwareMessage_Type_GPIOS_CHANGED)return false;
    } else if(port==meshtastic_PortNum_DETECTION_SENSOR_APP) {
      if(request||length>40)return false;
    } else return false;
    meshtastic_Data data{};
    data.portnum=static_cast<meshtastic_PortNum>(port);
    data.payload.size=pb_size_t(length);memcpy(data.payload.bytes,payload,length);
    data.request_id=request;data.has_bitfield=true;
    auto writer=pb_ostream_from_buffer(out,capacity);
    if(!pb_encode(&writer,meshtastic_Data_fields,&data))return false;
    size=writer.bytes_written;return true;
  }
  static bool replyData(const uint8_t *bytes,size_t size) {
    if(!bytes||!size||size>239)return false;
    meshtastic_Data data{};auto input=pb_istream_from_buffer(bytes,size);
    if(!pb_decode(&input,meshtastic_Data_fields,&data)||input.bytes_left||!data.request_id)return false;
    uint8_t expected[64]{};size_t length=0;
    return outgoing(number(data.portnum),data.payload.bytes,data.payload.size,data.request_id,
      expected,sizeof(expected),length)&&length==size&&!memcmp(expected,bytes,size);
  }
};
}
