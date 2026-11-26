// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MqttGatewayEdit.h"
#include "MqttGatewayConfigMapping.h"
#include "MqttRegionRootPolicy.h"
#include "../SettingsEdit.h"

namespace mt { namespace network {
// Companion-facing typed admin surface. Callers supply explicit build/runtime
// capability; no implicit global broker, trust store, or factory credentials.
class MqttGatewayAdmin {
  MqttGatewayEdit &edit_;
  SettingsEdit &core_;
  bool loaded_=false,regionRootRetryPending_=false;
public:
  MqttGatewayAdmin(MqttGatewayEdit&e,SettingsEdit&core):edit_(e),core_(core){}
  bool loaded(){if(loaded_||!edit_.ready())return false;loaded_=edit_.load();return loaded_;}
  bool ready()const{return loaded_&&edit_.ready();}
  bool remotePolicyWritable()const{return ready()&&!edit_.recovered();}
  bool canRetryIoError()const{return loaded_&&!edit_.dirty()&&edit_.ioError();}
  bool regionRootRetryPending()const{return regionRootRetryPending_;}
  void markRegionRootRetryPending(){regionRootRetryPending_=true;}
  // Raw staged edits remain visible if the journal becomes unready after the
  // edit was accepted; hiding this would make aggregate commit look clean.
  bool dirty()const{return edit_.dirty();}
  void runtimePolicy(bool supported,bool trustReady){edit_.runtimePolicy(supported,trustReady);}
  bool get(meshtastic_ModuleConfig_MQTTConfig &out,bool capable)const{
    return capable&&ready()&&MqttGatewayConfigMapping::toConfig(edit_.config(),out);
  }
  // Common Companion readback wrapper. MQTT configuration exists on
  // proxy-only profiles too; callers must not hide this route behind the
  // direct Wi-Fi MQTT runtime build flag.
  bool moduleConfig(meshtastic_ModuleConfig &out,unsigned tag,bool capable)const{
    if(tag!=meshtastic_ModuleConfig_mqtt_tag||!capable||!ready()||!edit_.config().valid())return false;
    // ModuleConfig is a large generated union. Fill the caller-owned output
    // only after all refusal checks, avoiding a second union-sized stack copy.
    out={};out.which_payload_variant=meshtastic_ModuleConfig_mqtt_tag;
    return MqttGatewayConfigMapping::toConfig(edit_.config(),out.payload_variant.mqtt);
  }
  int save(const meshtastic_ModuleConfig_MQTTConfig &input,bool capable,
           bool runtimeSupported,bool trustReady,uint32_t now){
    if(!capable)return -ENOTSUP;if(!ready())return -EACCES;
    edit_.runtimePolicy(runtimeSupported,trustReady);
    MqttGatewaySettings candidate;
    if(!MqttGatewayConfigMapping::fromConfig(input,edit_.config(),candidate))return -EINVAL;
    const int result=MqttGatewayConfigMapping::operationalStatus(candidate,runtimeSupported,trustReady);
    if(result)return result;
    const int saved=edit_.save(candidate,now,core_.open());
    if(!saved)core_.activity(now);
    return saved;
  }
  int saveRemote(const meshtastic_ModuleConfig_MQTTConfig &input,bool capable,
                 bool runtimeSupported,bool trustReady,uint32_t now){
    if(!remotePolicyWritable())return -EACCES;
    return save(input,capable,runtimeSupported,trustReady,now);
  }
  bool configOkToMqtt(bool capable)const{return capable&&ready()&&edit_.config().configOkToMqtt();}
  int saveConfigOkToMqtt(bool enabled,bool capable,uint32_t now){
    if(!capable)return -ENOTSUP;if(!ready())return -EACCES;
    MqttGatewaySettings candidate=edit_.config();
    if(!candidate.setConfigOkToMqtt(enabled))return -EINVAL;
    const int saved=edit_.save(candidate,now,core_.open());if(!saved)core_.activity(now);return saved;
  }
  bool channelUplinkEnabled(const Config &config,unsigned index,bool capable)const{
    return capable&&ready()&&edit_.config().channelUplinkEnabled(config,index);
  }
  int saveChannelUplink(const Config &config,unsigned index,bool enabled,bool capable,uint32_t now){
    if(!capable)return -ENOTSUP;if(!ready())return -EACCES;
    MqttGatewaySettings candidate=edit_.config();
    if(!candidate.setChannelUplink(config,index,enabled))return -EINVAL;
    const int saved=edit_.save(candidate,now,core_.open());if(!saved)core_.activity(now);return saved;
  }
  int saveRemoteChannelUplink(const Config &config,unsigned index,bool enabled,bool capable,uint32_t now){
    if(!remotePolicyWritable())return -EACCES;
    return saveChannelUplink(config,index,enabled,capable,now);
  }
  // Mirror stock's accepted-region housekeeping. The caller invokes this only
  // after the LoRa Config save succeeds; an open core edit stages this
  // ModuleConfig update so SettingsCommit owns its durable write.
  int applyRegionRootTopic(const char *regionName,size_t regionLength,
                           bool configOkToMqtt,uint32_t now,bool trackRetry=true){
    // This hook is reached only after the LoRa setting is already durable.
    // If the prior immediate MQTT write failed, reread the dual-slot journal
    // and retry only from an unambiguous non-recovered state.
    if(!ready()){
      const int retry=retryIoError();if(retry){if(trackRetry)regionRootRetryPending_=true;return retry;}
    }
    if(!ready()){if(trackRetry)regionRootRetryPending_=true;return -EACCES;}
    MqttGatewaySettings candidate=edit_.config();
    char nextRoot[MqttGatewaySettings::RootMax+1]{};
    const auto rootResult=MqttRegionRootPolicy::apply(
      reinterpret_cast<const char *>(candidate.bytes+MqttGatewaySettings::Address),
      candidate.bytes[MqttGatewaySettings::AddressLength],
      reinterpret_cast<const char *>(candidate.bytes+MqttGatewaySettings::Root),
      candidate.bytes[MqttGatewaySettings::RootLength],regionName,regionLength,
      nextRoot,sizeof(nextRoot));
    if(rootResult==MqttRegionRootPolicy::Result::Invalid)return -EINVAL;
    const bool rootChanged=rootResult==MqttRegionRootPolicy::Result::Changed;
    const bool consentChanged=candidate.configOkToMqtt()!=configOkToMqtt;
    if(!rootChanged&&!consentChanged){regionRootRetryPending_=false;return 0;}
    if(rootChanged){
      meshtastic_ModuleConfig_MQTTConfig input=meshtastic_ModuleConfig_MQTTConfig_init_zero;
      if(!MqttGatewayConfigMapping::toConfig(candidate,input))return -EINVAL;
      memset(input.root,0,sizeof(input.root));
      memcpy(input.root,nextRoot,strlen(nextRoot));
      if(!candidate.assign(input))return -EINVAL;
    }
    if(consentChanged&&!candidate.setConfigOkToMqtt(configOkToMqtt))return -EINVAL;
    const int status=MqttGatewayConfigMapping::operationalStatus(
      candidate,edit_.runtimeSupported(),edit_.trustReady());
    if(status){if(trackRetry)regionRootRetryPending_=true;return status;}
    const int saved=edit_.save(candidate,now,core_.open());
    if(!saved){core_.activity(now);regionRootRetryPending_=false;}
    else if(trackRetry)regionRootRetryPending_=true;
    return saved;
  }
  int applyRemoteRegionRootTopic(const char *regionName,size_t regionLength,
                                 bool configOkToMqtt,uint32_t now,bool trackRetry=true){
    // RF policy changes must not normalize a recovered journal: the older
    // selected slot may contain stale channel consent. Keep the existing
    // local path and its explicit IoError reread/retry behavior unchanged.
    if(edit_.recovered())return -EACCES;
    return applyRegionRootTopic(regionName,regionLength,configOkToMqtt,now,trackRetry);
  }
  int commit(uint32_t now){
    if(!edit_.dirty())return 0;
    return ready()?edit_.commit(now):-EACCES;
  }
  // Explicit retry path used only after an accepted LoRa region is already
  // durable and a prior immediate MQTT root write poisoned the journal.
  int retryIoError(){return loaded_?edit_.retryIoError():-EACCES;}
  bool busy()const{return core_.open();}
};
}}
