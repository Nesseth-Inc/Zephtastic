// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Config.h"
#include "MqttDefaultServerPolicy.h"
#include "meshtastic/module_config.pb.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt { namespace network {

// Canonical, bounded binary form of the pinned stock MQTTConfig fields.
// This component stores configuration only; runtime activation is separate.
class MqttGatewaySettings {
public:
  static constexpr size_t Size=580;
  static constexpr size_t AddressMax=63,UsernameMax=63,PasswordMax=31,RootMax=31;
  static constexpr size_t Flags=4,AddressLength=5,UsernameLength=6,PasswordLength=7,RootLength=8;
  static constexpr size_t PublishInterval=9,PositionPrecision=13,ReportLocation=17;
  static constexpr size_t Address=18,Username=82,Password=146,Root=178;
  static constexpr size_t ConsentBase=210,ChannelConsentSize=46,ChannelSlots=8,Crc=578;
  static constexpr uint8_t Enabled=1u<<0,Encryption=1u<<1,Json=1u<<2,Tls=1u<<3,
                           Proxy=1u<<4,MapReporting=1u<<5,HasMapSettings=1u<<6;
  uint8_t bytes[Size];

  MqttGatewaySettings(){memset(bytes,0,Size);bytes[0]='Z';bytes[1]='M';bytes[2]='S';bytes[3]=2;seal();}
  bool valid()const{
    if(bytes[0]!='Z'||bytes[1]!='M'||bytes[2]!='S'||bytes[3]!=2||
       bytes[AddressLength]>AddressMax||bytes[UsernameLength]>UsernameMax||
       bytes[PasswordLength]>PasswordMax||bytes[RootLength]>RootMax||bytes[ReportLocation]>1)return false;
    if(!validString(bytes+Address,64,bytes[AddressLength])||
       !validString(bytes+Username,64,bytes[UsernameLength])||
       !validString(bytes+Password,32,bytes[PasswordLength])||
       !validString(bytes+Root,32,bytes[RootLength]))return false;
    for(unsigned i=0;i<ChannelSlots;++i)if(!validConsent(bytes+ConsentBase+i*ChannelConsentSize))return false;
    const uint16_t stored=uint16_t(bytes[Crc])|uint16_t(uint16_t(bytes[Crc+1])<<8);
    return stored==checksum(bytes,Crc);
  }
  bool enabled()const{return valid()&&(bytes[Flags]&Enabled)!=0;}
  bool proxyEnabled()const{return valid()&&(bytes[Flags]&Proxy)!=0;}
  bool mapReportingEnabled()const{return valid()&&(bytes[Flags]&MapReporting)!=0;}
  bool mapSettingsConfigured()const{return valid()&&(bytes[Flags]&HasMapSettings)!=0;}
  bool mapLocationOptedIn()const{return valid()&&bytes[ReportLocation]!=0;}
  uint32_t mapPublishIntervalSeconds()const{return valid()?get32(bytes+PublishInterval):0;}
  uint32_t mapPositionPrecision()const{return valid()?get32(bytes+PositionPrecision):0;}
  bool encryptionEnabled()const{return valid()&&(bytes[Flags]&Encryption)!=0;}
  bool configOkToMqtt()const{return valid()&&(bytes[Flags]&ConfigOkToMqtt)!=0;}
  bool setConfigOkToMqtt(bool enabled){
    if(!valid())return false;MqttGatewaySettings next=*this;
    if(enabled)next.bytes[Flags]|=ConfigOkToMqtt;else next.bytes[Flags]&=uint8_t(~ConfigOkToMqtt);
    next.seal();if(!next.valid())return false;*this=next;return true;
  }
  // Consent is bound to slot plus exact effective role/name/key; a slot that
  // is reassigned cannot inherit permission from its former channel.
  bool channelUplinkEnabled(const Config &config,unsigned index)const{
    if(!valid()||index>=ChannelSlots)return false;
    uint8_t expected[ChannelConsentSize]{};if(!makeConsent(config,index,expected))return false;
    const uint8_t *actual=bytes+ConsentBase+index*ChannelConsentSize;
    expected[0]|=0x80u;return memcmp(actual,expected,ChannelConsentSize)==0;
  }
  bool setChannelUplink(const Config &config,unsigned index,bool enabled){
    if(!valid()||index>=ChannelSlots)return false;MqttGatewaySettings next=*this;
    uint8_t *slot=next.bytes+ConsentBase+index*ChannelConsentSize;memset(slot,0,ChannelConsentSize);
    if(enabled){if(!makeConsent(config,index,slot))return false;slot[0]|=0x80u;}
    next.seal();if(!next.valid())return false;*this=next;return true;
  }
  // Direct device MQTT requires its runtime and (for TLS) device trust.
  // Phone-proxy mode delegates broker TLS to the phone. Empty addresses use
  // stock's default broker at snapshot time; stored bytes remain unchanged.
  int admissionStatus(bool runtimeSupported=false,bool trustReady=false)const{
    if(!valid())return -EINVAL;
    // Stock retains json_enabled in MQTTConfig but always publishes the
    // binary ServiceEnvelope. Preserve the bit in storage/typed round trips;
    // it does not select a runtime wire format.
    const bool enabled=(bytes[Flags]&Enabled)!=0;
    const bool proxy=(bytes[Flags]&Proxy)!=0;
    if(!MqttDefaultServerPolicy::allowsConfiguredAddress(
         reinterpret_cast<const char *>(bytes+Address),bytes[AddressLength],
         (bytes[Flags]&Tls)!=0))return -EINVAL;
    if(enabled&&!proxy&&!runtimeSupported)return -ENOTSUP;
    if(enabled&&!proxy&&(bytes[Flags]&Tls)&&!trustReady)return -ENOTSUP;
    return 0;
  }
  bool assign(const meshtastic_ModuleConfig_MQTTConfig &input){
    if(!valid())return false;MqttGatewaySettings next=*this;
    size_t addressLength=0,usernameLength=0,passwordLength=0,rootLength=0;
    if(!boundedString(input.address,sizeof(input.address),AddressMax,addressLength)||
       !boundedString(input.username,sizeof(input.username),UsernameMax,usernameLength)||
       !boundedString(input.password,sizeof(input.password),PasswordMax,passwordLength)||
       !boundedString(input.root,sizeof(input.root),RootMax,rootLength))return false;
    memset(next.bytes+Address,0,64);memset(next.bytes+Username,0,64);
    memset(next.bytes+Password,0,32);memset(next.bytes+Root,0,32);
    next.bytes[Flags]=(next.bytes[Flags]&ConfigOkToMqtt)|(input.enabled?Enabled:0)|(input.encryption_enabled?Encryption:0)|
      (input.json_enabled?Json:0)|(input.tls_enabled?Tls:0)|
      (input.proxy_to_client_enabled?Proxy:0)|(input.map_reporting_enabled?MapReporting:0)|
      (input.has_map_report_settings?HasMapSettings:0);
    next.bytes[AddressLength]=uint8_t(addressLength);next.bytes[UsernameLength]=uint8_t(usernameLength);
    next.bytes[PasswordLength]=uint8_t(passwordLength);next.bytes[RootLength]=uint8_t(rootLength);
    memcpy(next.bytes+Address,input.address,addressLength);memcpy(next.bytes+Username,input.username,usernameLength);
    memcpy(next.bytes+Password,input.password,passwordLength);memcpy(next.bytes+Root,input.root,rootLength);
    put32(next.bytes+PublishInterval,input.map_report_settings.publish_interval_secs);
    put32(next.bytes+PositionPrecision,input.map_report_settings.position_precision);
    next.bytes[ReportLocation]=input.map_report_settings.should_report_location?1:0;
    next.seal();if(!next.valid())return false;*this=next;return true;
  }
  bool exportConfig(meshtastic_ModuleConfig_MQTTConfig &out)const{
    if(!valid())return false;meshtastic_ModuleConfig_MQTTConfig next=meshtastic_ModuleConfig_MQTTConfig_init_zero;
    next.enabled=(bytes[Flags]&Enabled)!=0;next.encryption_enabled=(bytes[Flags]&Encryption)!=0;
    next.json_enabled=(bytes[Flags]&Json)!=0;next.tls_enabled=(bytes[Flags]&Tls)!=0;
    next.proxy_to_client_enabled=(bytes[Flags]&Proxy)!=0;next.map_reporting_enabled=(bytes[Flags]&MapReporting)!=0;
    next.has_map_report_settings=(bytes[Flags]&HasMapSettings)!=0;
    memcpy(next.address,bytes+Address,size_t(bytes[AddressLength])+1);
    memcpy(next.username,bytes+Username,size_t(bytes[UsernameLength])+1);
    memcpy(next.password,bytes+Password,size_t(bytes[PasswordLength])+1);
    memcpy(next.root,bytes+Root,size_t(bytes[RootLength])+1);
    next.map_report_settings.publish_interval_secs=get32(bytes+PublishInterval);
    next.map_report_settings.position_precision=get32(bytes+PositionPrecision);
    next.map_report_settings.should_report_location=bytes[ReportLocation]!=0;
    out=next;return true;
  }

private:
  static constexpr uint8_t ConfigOkToMqtt=1u<<7;
  static bool makeConsent(const Config &config,unsigned index,uint8_t *out){
    if(!out||index>=ChannelSlots||!config.valid())return false;
    ChannelRecord record;if(!config.channelRecord(index,record))return false;
    const uint8_t role=uint8_t(index?2u:(config.channelConfigured()?1u:0u));
    if((index&&record.role()!=2)||(!index&&role!=1))return false;
    PrimaryChannel::Selection key;if(!config.channelSelection(index,key))return false;
    const char *name=index?(record.name()[0]?record.name():config.modemName()):config.effectivePrimaryName();
    size_t n=0;while(n<12&&name[n])++n;
    if(n==0||n>=12||key.length==0||key.length>32||!PrimaryChannel::validName(name,false)){
      PrimaryChannel::clear(key);return false;
    }
    memset(out,0,ChannelConsentSize);out[0]=role;out[1]=uint8_t(key.length);
    memcpy(out+2,name,n);memcpy(out+14,key.bytes,key.length);PrimaryChannel::clear(key);return true;
  }
  static bool validConsent(const uint8_t *p){
    if(p[0]==0){for(size_t i=1;i<ChannelConsentSize;++i)if(p[i])return false;return true;}
    if(p[0]!=0x81u&&p[0]!=0x82u)return false;
    const uint8_t keyLength=p[1];if(keyLength==0||keyLength>32)return false;
    bool terminated=false;
    for(unsigned i=2;i<14;++i){if(!p[i])terminated=true;else if(terminated||p[i]<0x20||p[i]>0x7e)return false;}
    if(!terminated)return false;
    for(unsigned i=14+keyLength;i<46;++i)if(p[i])return false;
    return true;
  }
  static bool boundedString(const char *p,size_t capacity,size_t maximum,size_t &length){
    if(!p||!capacity)return false;size_t n=0;while(n<capacity&&p[n])++n;
    if(n==capacity||n>maximum)return false;
    for(size_t i=n+1;i<capacity;++i)if(p[i])return false;
    length=n;return true;
  }
  static bool validString(const uint8_t *p,size_t capacity,size_t length){
    if(length>=capacity||p[length])return false;
    for(size_t i=0;i<length;++i)if(!p[i])return false;
    for(size_t i=length+1;i<capacity;++i)if(p[i])return false;
    return true;
  }
  static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
  static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
  static uint16_t checksum(const uint8_t *p,size_t n){uint16_t c=0xffff;for(size_t i=0;i<n;++i){c^=uint16_t(p[i])<<8;for(unsigned j=0;j<8;++j)c=c&0x8000?uint16_t((c<<1)^0x1021):uint16_t(c<<1);}return c;}
  void seal(){const uint16_t c=checksum(bytes,Crc);bytes[Crc]=uint8_t(c);bytes[Crc+1]=uint8_t(c>>8);}
};

}} // namespace mt::network
