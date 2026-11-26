// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../Config.h"
#include "meshtastic/module_config.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

namespace mt::modules {
// Independent supplemental record. Existing core/module/identity records are
// never resized, reinterpreted or erased. Entries are sorted canonical protobuf
// messages; absent entries deliberately fall back to their existing journal.
class CompletionSettings {
public:
  static constexpr size_t Size=1024,Header=8,WireCapacity=240,MaxEntries=9;
  static constexpr uint16_t PowerKey=0x8001;
  uint8_t bytes[Size]{};
private:
  static uint16_t get16(const uint8_t *p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
  static void put16(uint8_t *p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
  static bool moduleKey(unsigned key){
    return key==meshtastic_ModuleConfig_audio_tag||key==meshtastic_ModuleConfig_ambient_lighting_tag||
      key==meshtastic_ModuleConfig_paxcounter_tag||key==meshtastic_ModuleConfig_telemetry_tag||
      key==meshtastic_ModuleConfig_external_notification_tag||key==meshtastic_ModuleConfig_serial_tag||
      key==meshtastic_ModuleConfig_canned_message_tag||key==meshtastic_ModuleConfig_range_test_tag;
  }
  static bool canonical(uint16_t key,const uint8_t *p,size_t n){
    if(!p||!n||n>WireCapacity)return false;
    uint8_t encoded[WireCapacity]{};
    auto in=pb_istream_from_buffer(p,n);auto out=pb_ostream_from_buffer(encoded,sizeof(encoded));
    if(key==PowerKey){
      meshtastic_Config c{};
      if(!pb_decode(&in,meshtastic_Config_fields,&c)||in.bytes_left||c.which_payload_variant!=meshtastic_Config_power_tag||
         !pb_encode(&out,meshtastic_Config_fields,&c))return false;
    }else{
      if(!moduleKey(key))return false;
      meshtastic_ModuleConfig c{};
      if(!pb_decode(&in,meshtastic_ModuleConfig_fields,&c)||in.bytes_left||c.which_payload_variant!=key||
         !pb_encode(&out,meshtastic_ModuleConfig_fields,&c))return false;
    }
    return out.bytes_written==n&&!memcmp(encoded,p,n);
  }
  bool entry(uint16_t wanted,const uint8_t *&p,size_t &n)const{
    if(!valid())return false;
    size_t at=Header;
    for(size_t i=0;i<count();++i){
      const auto key=get16(bytes+at),length=get16(bytes+at+2);at+=4;
      if(key==wanted){p=bytes+at;n=length;return true;}
      at+=length;
    }
    return false;
  }
  bool assign(uint16_t key,const uint8_t *wire,size_t length){
    if(!valid()||!canonical(key,wire,length))return false;
    CompletionSettings next;size_t at=Header,write=Header;uint16_t entries=0;bool inserted=false;
    auto append=[&](uint16_t k,const uint8_t *p,size_t n){
      if(entries>=MaxEntries||n>WireCapacity||write+4+n>Size-2)return false;
      put16(next.bytes+write,k);put16(next.bytes+write+2,uint16_t(n));
      memcpy(next.bytes+write+4,p,n);write+=4+n;++entries;return true;
    };
    for(size_t i=0;i<count();++i){
      const auto k=get16(bytes+at),n=get16(bytes+at+2);at+=4;
      if(!inserted&&key<=k){if(!append(key,wire,length))return false;inserted=true;}
      if(k!=key&&!append(k,bytes+at,n))return false;
      at+=n;
    }
    if(!inserted&&!append(key,wire,length))return false;
    put16(next.bytes+4,uint16_t(write-Header));put16(next.bytes+6,entries);next.seal();
    if(!next.valid())return false;*this=next;return true;
  }
public:
  CompletionSettings(){bytes[0]='Z';bytes[1]='T';bytes[2]='C';bytes[3]=1;seal();}
  uint16_t used()const{return get16(bytes+4);}
  uint16_t count()const{return get16(bytes+6);}
  size_t storedPrefix()const{return Header+used();}
  void seal(){put16(bytes+Size-2,Config::checksum(bytes,Size-2));}
  bool valid()const{
    if(bytes[0]!='Z'||bytes[1]!='T'||bytes[2]!='C'||bytes[3]!=1||count()>MaxEntries||
       used()>Size-Header-2||get16(bytes+Size-2)!=Config::checksum(bytes,Size-2))return false;
    size_t at=Header;uint16_t last=0;
    for(size_t i=0;i<count();++i){
      if(at+4>Header+used())return false;
      const auto key=get16(bytes+at),n=get16(bytes+at+2);at+=4;
      if(key<=last||at+n>Header+used()||!canonical(key,bytes+at,n))return false;
      last=key;at+=n;
    }
    if(at!=Header+used())return false;
    for(size_t i=at;i<Size-2;++i)if(bytes[i])return false;
    return true;
  }
  bool same(const CompletionSettings &other)const{return !memcmp(bytes,other.bytes,Size);}
  bool get(meshtastic_ModuleConfig &out,unsigned tag)const{
    const uint8_t *p=nullptr;size_t n=0;if(!moduleKey(tag)||!entry(uint16_t(tag),p,n))return false;
    meshtastic_ModuleConfig value{};auto in=pb_istream_from_buffer(p,n);
    if(!pb_decode(&in,meshtastic_ModuleConfig_fields,&value)||in.bytes_left)return false;
    out=value;return true;
  }
  bool set(const meshtastic_ModuleConfig &value){
    if(!moduleKey(value.which_payload_variant))return false;
    uint8_t wire[WireCapacity]{};auto out=pb_ostream_from_buffer(wire,sizeof(wire));
    return pb_encode(&out,meshtastic_ModuleConfig_fields,&value)&&
      assign(uint16_t(value.which_payload_variant),wire,out.bytes_written);
  }
  bool power(meshtastic_Config_PowerConfig &out)const{
    const uint8_t *p=nullptr;size_t n=0;if(!entry(PowerKey,p,n))return false;
    meshtastic_Config c{};auto in=pb_istream_from_buffer(p,n);
    if(!pb_decode(&in,meshtastic_Config_fields,&c)||in.bytes_left)return false;
    out=c.payload_variant.power;return true;
  }
  bool setPower(const meshtastic_Config_PowerConfig &value){
    meshtastic_Config c{};c.which_payload_variant=meshtastic_Config_power_tag;c.payload_variant.power=value;
    uint8_t wire[WireCapacity]{};auto out=pb_ostream_from_buffer(wire,sizeof(wire));
    return pb_encode(&out,meshtastic_Config_fields,&c)&&assign(PowerKey,wire,out.bytes_written);
  }
};
} // namespace mt::modules
