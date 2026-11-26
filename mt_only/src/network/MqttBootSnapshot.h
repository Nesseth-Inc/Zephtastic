// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../stock/mqtt/Subscriptions.h"
#include "MqttGatewaySettings.h"
#include "MqttPublisherTransport.h"
#include "MqttTopicLevel.h"
#include "MtKeyCommand.h"
#include "../Config.h"
#include <stdio.h>

namespace mt::mqtt {
// Immutable per-boot broker policy; no factory broker or credentials.
class BootSnapshot {
  // Root is at most 31 bytes; ChannelRecord names are at most 11 bytes.
  // 31 + "/2/e/" + 11 + "/+" + NUL fits in 51 bytes.
  static constexpr size_t TopicCapacity=64;
  char host_[64]{},gateway_[10]{},user_[64]{},password_[32]{},root_[32]{};
  char subscriptionTopics_[Settings::MaxSubscriptionTopics][TopicCapacity]{};
  uint8_t subscriptionTopicCount_=0;
  Settings settings_{};
  static bool buildSubscriptionTopics(const char *root,const Config &channels,
                                      char out[Settings::MaxSubscriptionTopics][TopicCapacity],uint8_t &count) {
    count=0;
    if(!channels.valid())return false;
    const char *prefix=root&&*root?root:"msh";
    ChannelRecord record;
    auto getGlobalId = [&](size_t i) -> const char * {
      if (!channels.channelRecord(i, record) || !record.role()) return nullptr;
      return record.name()[0] ? record.name() : channels.modemName();
    };
    auto subscribe = [&](const char *name) {
      if (!name || count >= Settings::MaxSubscriptionTopics) return false;
      const size_t length = strnlen(name, MqttTopicLevel::MaxBytes + 1);
      if (!MqttTopicLevel::valid(name, length)) return false;
      const int n = snprintf(out[count], TopicCapacity, "%s/2/e/%s/+", prefix, name);
      if (n <= 0 || size_t(n) >= TopicCapacity) return false;
      for (size_t j = 0; j < count; ++j)
        if (!strcmp(out[j], out[count])) return true;
      ++count;
      return true;
    };
    return stock::MQTT::sendSubscriptions(network::MqttGatewaySettings::ChannelSlots,
        [&](size_t i) { return channels.channelDownlinkEnabled(i); }, getGlobalId, subscribe);
  }
public:
  static constexpr int SecurityTag=0x5a54;
  static bool broker(const char *address,bool tls,char *host,size_t capacity,uint16_t &port) {
    if(!address||!host||!capacity)return false;
    if(!*address) {
      const char *defaultHost=network::MqttDefaultServerPolicy::kDefaultHost;
      size_t n=0;while(defaultHost[n])++n;
      if(n>=capacity)return false;
      memcpy(host,defaultHost,n+1);
      port=uint16_t(tls?8883:1883);
      return true;
    }
    size_t length=0;
    while(address[length]&&address[length]!=':'){
      const auto c=static_cast<unsigned char>(address[length]);
      if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_')||
         length+1>=capacity)return false;
      ++length;
    }
    if(!length)return false;
    uint32_t parsed=tls?8883:1883;
    if(address[length]==':'){
      const char *p=address+length+1;if(!*p)return false;parsed=0;
      for(;*p;++p){if(*p<'0'||*p>'9')return false;parsed=parsed*10+unsigned(*p-'0');if(parsed>65535)return false;}
      if(!parsed)return false;
    }
    memcpy(host,address,length);host[length]=0;port=uint16_t(parsed);return true;
  }
  bool capture(const network::MqttGatewaySettings &config,uint32_t node,bool supported,bool trustReady,
               const Config &channels) {
    if(!config.valid()||!config.enabled()||config.admissionStatus(supported,trustReady)||node<=1||node>=0xfffffffeu)return false;
    meshtastic_ModuleConfig_MQTTConfig typed{};
    if(!config.exportConfig(typed)||!broker(typed.address,typed.tls_enabled,host_,sizeof(host_),settings_.port)){
      bench::wipeKeyBuffer(&typed,sizeof(typed));return false;
    }
    const char *storedAddress=reinterpret_cast<const char *>(
      config.bytes+network::MqttGatewaySettings::Address);
    const size_t storedAddressLength=config.bytes[network::MqttGatewaySettings::AddressLength];
    if(!network::MqttDefaultServerPolicy::allowsBrokerPort(
         network::MqttDefaultServerPolicy::isDefaultAddress(storedAddress,storedAddressLength),
         settings_.port)){
      bench::wipeKeyBuffer(&typed,sizeof(typed));return false;
    }
    for(size_t i=0;typed.root[i];++i)
      if(static_cast<unsigned char>(typed.root[i])<0x20||typed.root[i]=='+'||typed.root[i]=='#'){
        bench::wipeKeyBuffer(&typed,sizeof(typed));return false;
      }
    memcpy(user_,typed.username,sizeof(user_));memcpy(password_,typed.password,sizeof(password_));
    memcpy(root_,typed.root,sizeof(root_));
    if(!buildSubscriptionTopics(root_,channels,subscriptionTopics_,subscriptionTopicCount_)){
      bench::wipeKeyBuffer(&typed,sizeof(typed));return false;
    }
    const int size=snprintf(gateway_,sizeof(gateway_),"!%08x",unsigned(node));
    bench::wipeKeyBuffer(&typed,sizeof(typed));if(size!=9)return false;
    settings_.enabled=true;settings_.tls=(config.bytes[network::MqttGatewaySettings::Flags]&network::MqttGatewaySettings::Tls)!=0;
    settings_.proxyToClient=config.proxyEnabled();
    bool wantsLink=config.mapReportingEnabled();
    for(unsigned i=0;i<network::MqttGatewaySettings::ChannelSlots;++i)
      wantsLink=wantsLink||config.channelUplinkEnabled(channels,i)||channels.channelDownlinkEnabled(i);
    settings_.wantsLink=settings_.enabled&&wantsLink;
    settings_.host=host_;settings_.clientId=gateway_;settings_.username=user_;settings_.password=password_;
    settings_.subscriptionTopicCount=subscriptionTopicCount_;
    for(size_t i=0;i<subscriptionTopicCount_;++i)settings_.subscriptionTopics[i]=subscriptionTopics_[i];
    settings_.trust={settings_.tls,SecurityTag};return true;
  }
  bool downlinkSubscriptionsMatch(const Config &channels)const {
    char expected[Settings::MaxSubscriptionTopics][TopicCapacity]{};uint8_t count=0;
    if(!buildSubscriptionTopics(root_,channels,expected,count)||count!=subscriptionTopicCount_)return false;
    for(size_t i=0;i<count;++i)if(strcmp(expected[i],subscriptionTopics_[i]))return false;
    return true;
  }
  const Settings &settings()const{return settings_;}
  bool proxyEnabled()const{return settings_.proxyToClient;}
  const char *root()const{return root_;}
  const char *gateway()const{return gateway_;}
  uint8_t subscriptionTopicCount()const{return subscriptionTopicCount_;}
  const char *subscriptionTopic(size_t index)const{return index<subscriptionTopicCount_?subscriptionTopics_[index]:nullptr;}
};

class UplinkWindow {
  struct Entry{uint32_t source=0,id=0;uint64_t at=0;uint8_t slot=0;}entries_[16]{};
  unsigned next_=0;
public:
  bool contains(uint32_t from,uint32_t id,unsigned slot,uint64_t now)const {
    for(const auto &entry:entries_)if(entry.source==from&&entry.id==id&&entry.slot==slot&&
      now>=entry.at&&now-entry.at<60000)return true;
    return false;
  }
  void accepted(uint32_t from,uint32_t id,unsigned slot,uint64_t now) {
    entries_[next_]={from,id,now,uint8_t(slot)};next_=(next_+1)%16;
  }
};

// A successful setter can be an inert no-op. Restart only for a verified
// journal generation change or a staged edit, and never after failed commit.
class RestartPolicy {
  uint32_t bootGeneration_=0;
  bool durableChanged_=false,staged_=false;
public:
  void boot(uint32_t generation){bootGeneration_=generation;durableChanged_=staged_=false;}
  void observed(uint32_t generation,bool dirty){durableChanged_=generation!=bootGeneration_;staged_=dirty;}
  void saved(int result,uint32_t before,uint32_t after,bool dirty) {
    (void)before;if(!result)observed(after,dirty);
  }
  bool pending()const{return durableChanged_||staged_;}
  bool due(int commitResult,bool editing,bool dirty)const {
    return pending()&&!commitResult&&!editing&&!dirty;
  }
};
}
