// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "TrafficManagementSettings.h"
#include <pb_encode.h>
#include <pb_decode.h>

namespace mt::modules {
// New independent record. Never reinterpret the existing 112-byte module
// journal, core configuration, operating identity, keys or message history.
// [4..23] raw traffic settings; [24..25] beacon protobuf length (LE16);
// [26..267] canonical beacon protobuf; [268..272] send marker + UTC (0 unknown);
// [273..290] canned config; [291..312] serial config; [313] TAK team,
// [314] TAK role, [315..317] reserved zero;
// [318..319] Config CRC16.
struct ExtendedModuleSettings {
  static constexpr size_t Size=320,BeaconCapacity=242;
  static constexpr uint8_t Version=3,LegacyVersion=1,LegacyV2Version=2;
  static constexpr size_t CannedEnabled=273,CannedBell=274,CannedSource=275;
  static constexpr size_t SerialEnabled=291,SerialEcho=292,SerialRxd=293,
    SerialTxd=297,SerialBaud=301,SerialTimeout=305,SerialMode=309,ReservedStart=313;
  static constexpr size_t TakTeam=313,TakRole=314,TakReservedStart=315;
  uint8_t bytes[Size]{};
private:
  static uint16_t get16(const uint8_t *p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
  static void put16(uint8_t *p,uint16_t n){p[0]=uint8_t(n);p[1]=uint8_t(n>>8);}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  template<class E>static bool enumRange(const E &value,uint32_t maximum){
    using U=__underlying_type(E);
    static_assert(sizeof(U)<=sizeof(uint32_t));
    U raw{};memcpy(&raw,&value,sizeof(raw));
    return uint64_t(raw)<=maximum; // also rejects negative signed representations
  }
  template<class E>static uint32_t enumValue(const E &value){
    using U=__underlying_type(E);U raw{};memcpy(&raw,&value,sizeof(raw));return uint32_t(raw);
  }
  static bool beaconSupported(const meshtastic_ModuleConfig_MeshBeaconConfig &b){
    // Offers are advisory data only. Alternate radio targets need a joined
    // retune/restore owner and are rejected until that separate adapter exists.
    if((b.flags&~uint32_t(7))||b.broadcast_targets_count>4||
       strnlen(b.broadcast_message,sizeof(b.broadcast_message))==sizeof(b.broadcast_message)||
       !enumRange(b.broadcast_offer_region,uint32_t(_meshtastic_Config_LoRaConfig_RegionCode_MAX))||
       !enumRange(b.broadcast_offer_preset,uint32_t(_meshtastic_Config_LoRaConfig_ModemPreset_MAX)))return false;
    for(unsigned i=0;i<b.broadcast_targets_count;++i){const auto &target=b.broadcast_targets[i];
      if(!enumRange(target.preset,uint32_t(_meshtastic_Config_LoRaConfig_ModemPreset_MAX))||
         !enumRange(target.region,uint32_t(_meshtastic_Config_LoRaConfig_RegionCode_MAX)))return false;
    }
    if(b.has_broadcast_offer_channel){
      const auto &c=b.broadcast_offer_channel;
      if(strnlen(c.name,sizeof(c.name))==sizeof(c.name)||
         !(c.psk.size==0||c.psk.size==1||c.psk.size==16||c.psk.size==32)||
         (c.has_module_settings&&c.module_settings.position_precision>32))return false;
    }
    return true;
  }
  static bool beaconSlotValid(uint32_t slot,uint32_t region,bool hasPreset,uint32_t preset){
    if(!slot)return false;
    if(!region||!hasPreset)return true;
    // An explicit supported region/preset has a fixed slot count. A target
    // using the current radio is checked when the wave is actually sent.
    RadioSettings::Request radio{};radio.regionCode=region;radio.preset=preset;radio.channel=0;
    const char *name=Config::radioPresetName(preset);
    LoRaPresets::Profile modem{};USChannelSelection derived{};
    return !name||!RadioSettings::resolve(radio,modem,derived,name,name)||slot<=derived.slotCount;
  }
  bool encodeBeacon(meshtastic_ModuleConfig_MeshBeaconConfig b){
    if(!beaconSupported(b))return false;
    if(b.broadcast_interval_secs&&b.broadcast_interval_secs<3600)b.broadcast_interval_secs=3600;
    for(unsigned i=0;i<b.broadcast_targets_count;++i){
      auto &t=b.broadcast_targets[i];
      if(t.has_frequency_slot&&!beaconSlotValid(t.frequency_slot,t.region,t.has_preset,t.preset))
        t.has_frequency_slot=false;
    }
    if(b.has_broadcast_offer_frequency_slot&&!beaconSlotValid(b.broadcast_offer_frequency_slot,
         b.broadcast_offer_region,b.has_broadcast_offer_preset,b.broadcast_offer_preset))
      b.has_broadcast_offer_frequency_slot=false;
    uint8_t encoded[BeaconCapacity]{};
    auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));
    if(!pb_encode(&stream,meshtastic_ModuleConfig_MeshBeaconConfig_fields,&b))return false;
    memset(bytes+26,0,BeaconCapacity);memcpy(bytes+26,encoded,stream.bytes_written);
    put16(bytes+24,uint16_t(stream.bytes_written));return true;
  }
  static bool boolByte(uint8_t value){return value<=1;}
  static bool validInputSource(const char *source,size_t capacity){
    const size_t n=strnlen(source,capacity);
    if(n==capacity||(n&&!(n==4&&!memcmp(source,"_any",4))))return false;
    for(size_t i=n;i<capacity;++i)if(source[i])return false;
    return true;
  }
  static bool cannedConfigSupported(const meshtastic_ModuleConfig_CannedMessageConfig &c){
    return !c.rotary1_enabled&&!c.inputbroker_pin_a&&!c.inputbroker_pin_b&&!c.inputbroker_pin_press&&
      enumRange(c.inputbroker_event_cw,0)&&enumRange(c.inputbroker_event_ccw,0)&&
      enumRange(c.inputbroker_event_press,0)&&!c.updown1_enabled&&
      validInputSource(c.allow_input_source,sizeof(c.allow_input_source));
  }
  static bool serialConfigSupported(const meshtastic_ModuleConfig_SerialConfig &s){
    if(s.override_console_serial_port||
       !enumRange(s.baud,uint32_t(_meshtastic_ModuleConfig_SerialConfig_Serial_Baud_MAX)))return false;
    const auto raw=enumValue(s.mode);
    return raw==uint32_t(meshtastic_ModuleConfig_SerialConfig_Serial_Mode_DEFAULT)||
      raw==uint32_t(meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE)||
      raw==uint32_t(meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG);
  }
  bool moduleBytesValid(bool hasTak)const{
    if(!boolByte(bytes[CannedEnabled])||!boolByte(bytes[CannedBell])||
       !validInputSource(reinterpret_cast<const char *>(bytes+CannedSource),16)||
       !boolByte(bytes[SerialEnabled])||!boolByte(bytes[SerialEcho]))return false;
    const uint32_t baud=get32(bytes+SerialBaud),mode=get32(bytes+SerialMode);
    if(baud>uint32_t(_meshtastic_ModuleConfig_SerialConfig_Serial_Baud_MAX))return false;
    if(mode!=uint32_t(meshtastic_ModuleConfig_SerialConfig_Serial_Mode_DEFAULT)&&
       mode!=uint32_t(meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE)&&
       mode!=uint32_t(meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG))return false;
    if(hasTak){
      if(bytes[TakTeam]>uint8_t(meshtastic_Team_Brown)||
         bytes[TakRole]>uint8_t(meshtastic_MemberRole_K9))return false;
      for(size_t i=TakReservedStart;i<318;++i)if(bytes[i])return false;
    }else for(size_t i=ReservedStart;i<318;++i)if(bytes[i])return false;
    return true;
  }
  bool validAs(uint8_t version,bool hasTak)const{
    if(bytes[0]!='Z'||bytes[1]!='T'||bytes[2]!='E'||bytes[3]!=version||bytes[268]>1||
       (!bytes[268]&&Config::get32(bytes+269))||get16(bytes+318)!=Config::checksum(bytes,Size-2))return false;
    if(!moduleBytesValid(hasTak))return false;
    meshtastic_ModuleConfig_MeshBeaconConfig b{};return beacon(b);
  }
public:
  ExtendedModuleSettings(){
    bytes[0]='Z';bytes[1]='T';bytes[2]='E';bytes[3]=Version;
    TrafficManagementSettings traffic;memcpy(bytes+4,traffic.bytes,traffic.Size);
    meshtastic_ModuleConfig_MeshBeaconConfig b{};b.flags=5; // stock listen + legacy split; no broadcast
    if(!encodeBeacon(b))bytes[0]=0;
    seal();
  }
  void seal(){const auto crc=Config::checksum(bytes,Size-2);put16(bytes+Size-2,crc);}
  // Read a CRC-verified 2.8.1 journal or preference backup. Only the stored
  // beacon text shrank. Retain every other field and canonical padding.
  bool loadStored(){
    if(get16(bytes+318)!=Config::checksum(bytes,Size-2))return false;
    const size_t n=get16(bytes+24);
    if(n>BeaconCapacity)return false;
    auto *wire=bytes+26;
    auto input=pb_istream_from_buffer(wire,n);
    while(input.bytes_left){
      pb_wire_type_t type;uint32_t tag;bool eof;
      if(!pb_decode_tag(&input,&type,&tag,&eof))return false;
      if(tag==meshtastic_ModuleConfig_MeshBeaconConfig_broadcast_message_tag && type==PB_WT_STRING){
        const size_t lenAt=n-input.bytes_left;
        if(!input.bytes_left)return false;
        const size_t len=wire[lenAt],limit=sizeof(meshtastic_ModuleConfig_MeshBeaconConfig::broadcast_message)-1;
        if(len>limit){
          if(len>100||input.bytes_left<=len||memchr(wire+lenAt+1,0,len))return false;
          size_t keep=limit;
          while(keep && (wire[lenAt+1+keep]&0xc0)==0x80)--keep;
          const size_t removed=len-keep;
          wire[lenAt]=uint8_t(keep);
          memmove(wire+lenAt+1+keep,wire+lenAt+1+len,BeaconCapacity-lenAt-1-len);
          memset(wire+BeaconCapacity-removed,0,removed);
          put16(bytes+24,uint16_t(n-removed));seal();
        }
        break;
      }
      if(!pb_skip_field(&input,type))return false;
    }
    if(bytes[3]==LegacyVersion)return migrateV1();
    if(bytes[3]==LegacyV2Version)return migrateV2();
    return valid();
  }
  bool migrateV1(){
    if(bytes[0]!='Z'||bytes[1]!='T'||bytes[2]!='E'||bytes[3]!=LegacyVersion||bytes[268]>1||
       (!bytes[268]&&Config::get32(bytes+269))||get16(bytes+318)!=Config::checksum(bytes,Size-2))return false;
    for(size_t i=273;i<318;++i)if(bytes[i])return false;
    bytes[3]=Version;bytes[CannedEnabled]=0;bytes[CannedBell]=0;
    memset(bytes+CannedSource,0,16);memset(bytes+SerialEnabled,0,ReservedStart-SerialEnabled);
    seal();return valid();
  }
  bool migrateV2(){
    // Validate the exact old inner-v2 record, including its original CRC and
    // zero-reserved TAK bytes, before assigning the new enum defaults.
    if(!validAs(LegacyV2Version,false))return false;
    bytes[3]=Version;bytes[TakTeam]=uint8_t(meshtastic_Team_Unspecifed_Color);
    bytes[TakRole]=uint8_t(meshtastic_MemberRole_Unspecifed);
    seal();return valid();
  }
  bool beacon(meshtastic_ModuleConfig_MeshBeaconConfig &out)const {
    out={};const size_t n=get16(bytes+24);if(n>BeaconCapacity)return false;
    auto stream=pb_istream_from_buffer(bytes+26,n);
    if(!pb_decode(&stream,meshtastic_ModuleConfig_MeshBeaconConfig_fields,&out)||!beaconSupported(out))return false;
    // A valid stored record has a unique canonical encoding and zero padding.
    uint8_t encoded[BeaconCapacity]{};auto writer=pb_ostream_from_buffer(encoded,sizeof(encoded));
    return pb_encode(&writer,meshtastic_ModuleConfig_MeshBeaconConfig_fields,&out)&&
      writer.bytes_written==n&&!memcmp(encoded,bytes+26,BeaconCapacity);
  }
  bool valid()const {
    return validAs(Version,true);
  }
  bool same(const ExtendedModuleSettings &other)const{return !memcmp(bytes,other.bytes,Size);}
  TrafficManagementSettings traffic()const {
    TrafficManagementSettings out;memcpy(out.bytes,bytes+4,out.Size);return out;
  }
  bool get(meshtastic_ModuleConfig &out,unsigned tag)const {
    out={};if(!valid())return false;
    out.which_payload_variant=tag;
    if(tag==meshtastic_ModuleConfig_canned_message_tag)return canned(out.payload_variant.canned_message);
    if(tag==meshtastic_ModuleConfig_serial_tag)return serial(out.payload_variant.serial);
    if(tag==meshtastic_ModuleConfig_tak_tag)return tak(out.payload_variant.tak);
    if(tag==meshtastic_ModuleConfig_traffic_management_tag){traffic().get(out.payload_variant.traffic_management);return true;}
    if(tag==meshtastic_ModuleConfig_mesh_beacon_tag)return beacon(out.payload_variant.mesh_beacon);
    out={};return false;
  }
  bool canned(meshtastic_ModuleConfig_CannedMessageConfig &out)const{
    out={};if(!valid())return false;
    out.enabled=bytes[CannedEnabled]!=0;out.send_bell=bytes[CannedBell]!=0;
    memcpy(out.allow_input_source,bytes+CannedSource,sizeof(out.allow_input_source));return true;
  }
  bool serial(meshtastic_ModuleConfig_SerialConfig &out)const{
    out={};if(!valid())return false;
    out.enabled=bytes[SerialEnabled]!=0;out.echo=bytes[SerialEcho]!=0;
    out.rxd=get32(bytes+SerialRxd);out.txd=get32(bytes+SerialTxd);
    const uint32_t baud=get32(bytes+SerialBaud),mode=get32(bytes+SerialMode);
    out.baud=static_cast<meshtastic_ModuleConfig_SerialConfig_Serial_Baud>(baud);
    out.timeout=get32(bytes+SerialTimeout);
    out.mode=static_cast<meshtastic_ModuleConfig_SerialConfig_Serial_Mode>(mode);return true;
  }
  bool tak(meshtastic_ModuleConfig_TAKConfig &out)const{
    out={};if(!valid())return false;
    out.team=static_cast<meshtastic_Team>(bytes[TakTeam]);
    out.role=static_cast<meshtastic_MemberRole>(bytes[TakRole]);return true;
  }
  bool cannedEnabled()const{return valid()&&bytes[CannedEnabled]!=0;}
  bool cannedBell()const{return valid()&&bytes[CannedBell]!=0;}
  bool setCanned(const meshtastic_ModuleConfig_CannedMessageConfig &c){
    if(!valid()||!cannedConfigSupported(c))return false;
    auto candidate=*this;candidate.bytes[CannedEnabled]=c.enabled?1:0;candidate.bytes[CannedBell]=c.send_bell?1:0;
    memset(candidate.bytes+CannedSource,0,16);memcpy(candidate.bytes+CannedSource,c.allow_input_source,sizeof(c.allow_input_source));
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setSerial(const meshtastic_ModuleConfig_SerialConfig &s){
    if(!valid()||!serialConfigSupported(s))return false;
    auto candidate=*this;candidate.bytes[SerialEnabled]=s.enabled?1:0;candidate.bytes[SerialEcho]=s.echo?1:0;
    put32(candidate.bytes+SerialRxd,s.rxd);put32(candidate.bytes+SerialTxd,s.txd);
    put32(candidate.bytes+SerialBaud,static_cast<uint32_t>(s.baud));
    put32(candidate.bytes+SerialTimeout,s.timeout);put32(candidate.bytes+SerialMode,static_cast<uint32_t>(s.mode));
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setTak(const meshtastic_ModuleConfig_TAKConfig &t){
    if(!valid()||!enumRange(t.team,uint32_t(meshtastic_Team_Brown))||
       !enumRange(t.role,uint32_t(meshtastic_MemberRole_K9)))return false;
    auto candidate=*this;candidate.bytes[TakTeam]=uint8_t(enumValue(t.team));
    candidate.bytes[TakRole]=uint8_t(enumValue(t.role));candidate.seal();
    if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool set(const meshtastic_ModuleConfig &in){
    if(!valid())return false;
    auto candidate=*this;
    if(in.which_payload_variant==meshtastic_ModuleConfig_canned_message_tag){
      if(!candidate.setCanned(in.payload_variant.canned_message))return false;
    }else if(in.which_payload_variant==meshtastic_ModuleConfig_serial_tag){
      if(!candidate.setSerial(in.payload_variant.serial))return false;
    }else if(in.which_payload_variant==meshtastic_ModuleConfig_tak_tag){
      if(!candidate.setTak(in.payload_variant.tak))return false;
    }else if(in.which_payload_variant==meshtastic_ModuleConfig_traffic_management_tag){
      TrafficManagementSettings t;t.set(in.payload_variant.traffic_management);memcpy(candidate.bytes+4,t.bytes,t.Size);
    }else if(in.which_payload_variant==meshtastic_ModuleConfig_mesh_beacon_tag){
      if(!candidate.encodeBeacon(in.payload_variant.mesh_beacon))return false;
    }else return false;
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool sentBefore()const{return bytes[268]==1;}
  uint32_t lastSentUtc()const{return Config::get32(bytes+269);}
  void markSent(uint32_t utc){bytes[268]=1;Config::put32(bytes+269,utc);seal();}
};
static_assert(sizeof(ExtendedModuleSettings)==ExtendedModuleSettings::Size);
static_assert(meshtastic_ModuleConfig_MeshBeaconConfig_size<=ExtendedModuleSettings::BeaconCapacity);
}
