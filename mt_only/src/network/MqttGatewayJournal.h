// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MqttGatewaySettings.h"
#include "../SettingsJournal.h"

namespace mt { namespace network {

// Isolated dual-slot store. It has no migration path and never reads/writes
// mtbase or mtmod keys. Runtime admission is performed by MqttGatewayEdit.
class MqttGatewayJournal {
public:
  static constexpr size_t RecordSize=12+MqttGatewaySettings::Size+4;
  enum class State:uint8_t {Unloaded,Fresh,Loaded,Recovered,Invalid,IoError};
private:
  MqttGatewaySettings config_;
  uint32_t generation_=0;
  int8_t active_=-1;
  State state_=State::Unloaded;
  static const char *key(unsigned slot){return slot?"mtmqtt/state1":"mtmqtt/state0";}
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static uint16_t get16(const uint8_t *p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint32_t crc32(const uint8_t *p,size_t n){
    uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;
  }
  static void encode(const MqttGatewaySettings &config,uint32_t generation,uint8_t *out){
    memset(out,0,RecordSize);out[0]='Z';out[1]='T';out[2]='M';out[3]=2;put32(out+4,generation);
    out[8]=uint8_t(MqttGatewaySettings::Size);out[9]=uint8_t(MqttGatewaySettings::Size>>8);
    memcpy(out+12,config.bytes,MqttGatewaySettings::Size);put32(out+RecordSize-4,crc32(out,RecordSize-4));
  }
  static bool decode(const uint8_t *p,int n,MqttGatewaySettings &config,uint32_t &generation){
    if(n!=int(RecordSize)||p[0]!='Z'||p[1]!='T'||p[2]!='M'||p[3]!=2||
       get16(p+8)!=MqttGatewaySettings::Size||p[10]||p[11]||
       get32(p+RecordSize-4)!=crc32(p,RecordSize-4))return false;
    MqttGatewaySettings candidate;memcpy(candidate.bytes,p+12,MqttGatewaySettings::Size);
    if(!candidate.valid())return false;
    config=candidate;generation=get32(p+4);return true;
  }
  int saveVerified(SettingsBackend &backend,const MqttGatewaySettings &candidate){
    if(!ready())return -EACCES;
    if(!config_.valid()||!candidate.valid())return -EINVAL;
    if(active_>=0&&memcmp(config_.bytes,candidate.bytes,MqttGatewaySettings::Size)==0)return 0;
    const unsigned slot=active_==0?1:0;const uint32_t next=generation_+1;
    uint8_t record[RecordSize],readback[RecordSize];encode(candidate,next,record);
    const int written=backend.write(key(slot),record,sizeof(record));
    if(written){state_=State::IoError;return written;}
    const int n=backend.read(key(slot),readback,sizeof(readback));
    if(n!=int(sizeof(record))||memcmp(record,readback,sizeof(record))){state_=State::IoError;return n<0?n:-EIO;}
    config_=candidate;generation_=next;active_=int8_t(slot);state_=State::Loaded;return 0;
  }
public:
  const MqttGatewaySettings &config()const{return config_;}
  State state()const{return state_;}
  uint32_t generation()const{return generation_;}
  int activeSlot()const{return active_;}
  bool ready()const{return state_==State::Fresh||state_==State::Loaded||state_==State::Recovered;}
  int load(SettingsBackend &backend){
    config_=MqttGatewaySettings{};generation_=0;active_=-1;state_=State::Unloaded;
    uint8_t raw[RecordSize];MqttGatewaySettings candidates[2];uint32_t generations[2]{};bool good[2]{},present[2]{};
    for(unsigned i=0;i<2;++i){
      const int n=backend.read(key(i),raw,sizeof(raw));
      if(n<0&&n!=-ENOENT&&n!=-E2BIG){state_=State::IoError;return n;}
      present[i]=n!=-ENOENT;good[i]=decode(raw,n,candidates[i],generations[i]);
    }
    if(good[0]||good[1]){
      unsigned slot=good[1]?1:0;
      if(good[0]&&good[1]){
        const uint32_t delta=generations[1]-generations[0];
        if((!delta&&memcmp(candidates[0].bytes,candidates[1].bytes,MqttGatewaySettings::Size)!=0)||delta==0x80000000u){state_=State::Invalid;return -EINVAL;}
        slot=delta&&delta<0x80000000u?1:0;
      }
      config_=candidates[slot];generation_=generations[slot];active_=int8_t(slot);
      state_=(present[0]&&!good[0])||(present[1]&&!good[1])?State::Recovered:State::Loaded;return 0;
    }
    if(!present[0]&&!present[1]){state_=State::Fresh;return 0;}
    state_=State::Invalid;return -EINVAL;
  }
  int save(SettingsBackend &backend,const MqttGatewaySettings &candidate,
           bool runtimeSupported=false,bool trustReady=false){
    if(!ready())return -EACCES;
    if(!candidate.valid())return -EINVAL;
    const int support=candidate.admissionStatus(runtimeSupported,trustReady);if(support)return support;
    return saveVerified(backend,candidate);
  }
  // Narrow pre-runtime consent persistence for the multi-journal role intent.
  // Broker, credentials, TLS, proxy and map settings are immutable through
  // this path. It does not run admissionStatus(), infer runtime support, or
  // activate the MQTT runtime; the caller still owns the outer transaction.
  int saveChannelConsentCandidate(SettingsBackend &backend,const MqttGatewaySettings &candidate){
    if(!ready())return -EACCES;
    if(!config_.valid()||!candidate.valid())return -EINVAL;
    if(memcmp(config_.bytes,candidate.bytes,MqttGatewaySettings::ConsentBase)!=0)return -EPERM;
    return saveVerified(backend,candidate);
  }
  // Narrow managed-LoRa policy/root projection path. Only the ConfigOkToMqtt
  // bit, root length/string, and record seal may differ. It does not admit
  // broker/credential/TLS/runtime changes or activate MQTT.
  int saveLoRaPolicyCandidate(SettingsBackend &backend,const MqttGatewaySettings &candidate){
    if(!ready())return -EACCES;
    if(!config_.valid()||!candidate.valid())return -EINVAL;
    constexpr uint8_t ConfigOkBit=0x80u;
    for(size_t i=0;i<MqttGatewaySettings::Crc;++i) {
      if(i==MqttGatewaySettings::Flags) {
        if((config_.bytes[i]&uint8_t(~ConfigOkBit))!=
           (candidate.bytes[i]&uint8_t(~ConfigOkBit)))return -EPERM;
      } else if(i==MqttGatewaySettings::RootLength||
         (i>=MqttGatewaySettings::Root&&i<MqttGatewaySettings::Root+32)) {
        continue;
      } else if(config_.bytes[i]!=candidate.bytes[i])return -EPERM;
    }
    return saveVerified(backend,candidate);
  }
};

}} // namespace mt::network
