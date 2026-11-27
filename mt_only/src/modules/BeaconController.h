// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "BeaconCadence.h"
#include "SameChannelBeaconPayloads.h"
#include "ExtendedModuleEdit.h"
#include "BeaconTargetRuntime.h"

namespace mt::modules {
// For the full companion the target owner already retains both halves of
// a split beacon. Its queue validates settings and identity epochs before TX.
class BeaconBroadcaster {
protected:
  beacon::BeaconCadence cadence_;
  static bool idle(Service &service){
    return service.normalMode()&&service.receiving()&&service.endpointArmed()&&service.txAllowed()&&
      service.config.txEnabled()&&!service.faulted()&&!service.moduleAnnouncementPending()&&
      !service.endpointControls()&&service.endpointFree()==4&&service.storageStopSafe();
  }
  static bool encode(meshtastic_Data &d,uint8_t *wire,size_t &size){
    size=0;d.has_bitfield=true;d.bitfield=0;
    auto output=pb_ostream_from_buffer(wire,239);
    if(!pb_encode(&output,meshtastic_Data_fields,&d))return false;
    bool telemetry=false;
    if(!ModuleAnnouncement::canonical(wire,output.bytes_written,telemetry,true))return false;
    size=output.bytes_written;return true;
  }
public:
  static bool resolveTargets(const Config &core,const meshtastic_ModuleConfig_MeshBeaconConfig &cfg,
      BeaconExcursion::ResolvedTarget (&out)[4],size_t &count){
    const auto home=BeaconTargetRuntime::runningProfile(core);
    count=0;
    if(cfg.broadcast_targets_count>4)return false;
    for(unsigned i=0;i<(cfg.broadcast_targets_count?cfg.broadcast_targets_count:1u);++i){
      const meshtastic_ModuleConfig_MeshBeaconConfig_BroadcastTarget primary{};
      const auto &entry=cfg.broadcast_targets_count?cfg.broadcast_targets[i]:primary;
      auto profile=home;
      if(entry.has_preset){profile.preset=uint32_t(entry.preset);profile.usePreset=true;}
      if(entry.region)profile.region=uint32_t(entry.region);
      if(entry.has_channel_index && entry.channel_index>=8)continue;
      profile.channelIndex=entry.has_channel_index?entry.channel_index:0;
      ChannelRecord record{};
      if(!core.channelRecord(profile.channelIndex,record)||!record.role()||
         (entry.has_channel_index&&!record.name()[0]&&!record.keySize()))continue;
      auto request=core.radioRequest();request.regionCode=profile.region;
      request.preset=profile.preset;request.usePreset=profile.usePreset;request.overrideMHz=0;
      if(!RadioSettings::normalizeRegionPreset(profile.region,profile.usePreset,profile.preset,profile.region))continue;
      const bool homeRadio=profile.region==home.region&&profile.preset==home.preset&&profile.usePreset==home.usePreset;
      request.channel=entry.has_frequency_slot&&entry.frequency_slot?entry.frequency_slot:(homeRadio?home.slot:0);
      const char *display=profile.usePreset?Config::radioPresetName(profile.preset):"Custom";
      LoRaPresets::Profile modem{};USChannelSelection selection{};
      if(!display||!RadioSettings::resolve(request,modem,selection,record.name()[0]?record.name():display,display))continue;
      profile.slot=selection.slot+1u;
      bool duplicate=false;
      for(size_t j=0;j<count;++j)
        if(profile.sameRadio(out[j].profile)&&profile.channelIndex==out[j].profile.channelIndex)duplicate=true;
      if(!duplicate)out[count++]={profile,!profile.sameRadio(home),bool(cfg.flags&4)};
    }
    return true;
  }
  static bool targetsSupported(const Config &core,const meshtastic_ModuleConfig_MeshBeaconConfig &cfg,BeaconTargetRuntime &runtime){
    BeaconExcursion::ResolvedTarget targets[4]{};size_t count=0;if(!resolveTargets(core,cfg,targets,count))return false;
    for(size_t i=0;i<count;++i)if(!runtime.targetSupported(targets[i].profile))return false;return true;
  }
  static int persistWave(Service &service,ExtendedModuleEdit &edit,uint32_t utc,uint32_t now){
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
    if(service.broadcastHistory&&service.broadcastHistory->readable()&&service.saveBeaconHistory)
      return service.saveBeaconHistory();
#else
    (void)service;
#endif
    return edit.markBeaconSent(utc,now);
  }
  bool due(Service &service,ExtendedModuleEdit &edit,const meshtastic_ModuleConfig_MeshBeaconConfig &cfg,
           uint64_t uptime,uint32_t utc){
    return idle(service)&&cadence_.takeScheduledTick(uptime,cfg.broadcast_interval_secs)&&
      stock::MeshBeaconBroadcastModule::broadcastDue(cfg,service.config.role(),service.moduleAirtimeAllowed(),[&]{
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
            if(service.broadcastHistory&&service.broadcastHistory->has(history::Kind::Beacon))
              return service.broadcastDue(history::Kind::Beacon,beacon::BeaconCadence::intervalMs(cfg.broadcast_interval_secs));
#endif
            return cadence_.due(uptime,utc,edit.config().sentBefore(),edit.config().lastSentUtc(),cfg.broadcast_interval_secs);
          });
  }
  template<class Sink>void pollTargets(Service &service,ExtendedModuleEdit &edit,bool ready,
      uint64_t uptime,uint32_t utc,uint32_t random,Sink sink,BeaconTargetRuntime &targets,uint32_t settingsEpoch){
    if(!ready||!edit.trusted()||uptime==UINT64_MAX||targets.busy())return;
    meshtastic_ModuleConfig_MeshBeaconConfig cfg{};
    if(!edit.config().beacon(cfg)||!due(service,edit,cfg,uptime,utc))return;
    BeaconExcursion::ResolvedTarget resolved[4]{};size_t count=0;
    if(!resolveTargets(service.config,cfg,resolved,count)||!count)return;
    size_t available=0;
    for(size_t i=0;i<count;++i){
      if(!targets.targetSupported(resolved[i].profile))continue;
      if(i!=available)resolved[available]=resolved[i];
      ++available;
    }
    if(!available)return;
    count=available;
    beacon::BeaconPayloadBatch batch{};
    if(!stock::MeshBeaconBroadcastModule::sendBeacon(cfg,batch,service.config.regionCode()))return;
    uint8_t payloads[2][239]{};size_t lengths[2]{};
    for(unsigned p=0;p<batch.count;++p)if(!encode(batch.packets[p],payloads[p],lengths[p]))return;
    // Same verified per-wave marker as the original path, before any enqueue.
    if(persistWave(service,edit,utc,uint32_t(uptime))||!cadence_.attempted(uptime))return;
    for(size_t i=0;i<count;++i)for(unsigned p=0;p<batch.count;++p){
      BeaconExcursion::Request request{};request.groupId=uint32_t(i+1);request.settingsEpoch=settingsEpoch;request.identityEpoch=service.route.identity();
      request.target=resolved[i].profile;request.requiresRetune=resolved[i].requiresRetune;request.legacyHopOverride=resolved[i].legacyHopOverride;
      uint32_t id=0;if(!targets.enqueue(request,payloads[p],lengths[p],random^uint32_t((i+1)*0x9e3779b9u+p),uint32_t(uptime),&id))return;
      sink(payloads[p],lengths[p],id,resolved[i].profile.channelIndex);
    }
  }
};

// Same-channel callers without an excursion queue retain a pending split.
class BeaconController:public BeaconBroadcaster {
  uint8_t pending_[239]{},config_[244]{},core_[Config::Size]{};size_t pendingSize_=0;uint64_t pendingAt_=0;
  void clear(){memset(pending_,0,sizeof(pending_));memset(config_,0,sizeof(config_));memset(core_,0,sizeof(core_));pendingSize_=0;pendingAt_=0;}
public:
  template<class Sink>void poll(Service &service,ExtendedModuleEdit &edit,bool ready,
      uint64_t uptime,uint32_t utc,uint32_t random,Sink sink,BeaconTargetRuntime *targets=nullptr,uint32_t settingsEpoch=0){
    if(targets){
      clear();pollTargets(service,edit,ready,uptime,utc,random,
        [&](const uint8_t *data,size_t size,uint32_t id,unsigned){sink(data,size,id);},*targets,settingsEpoch);
      return;
    }
    if(!ready||!edit.trusted()||uptime==UINT64_MAX){clear();return;}
    meshtastic_ModuleConfig_MeshBeaconConfig cfg{};
    if(!edit.config().beacon(cfg)){clear();return;}
    if(!pendingSize_){
      if(!due(service,edit,cfg,uptime,utc))return;
    }else if(!stock::MeshBeaconBroadcastModule::enabled(cfg,service.config.role())){clear();return;}
    if(pendingSize_){
      // A non-batch core settings save can change the primary between halves.
      // Cancel on any core change so the legacy text never crosses contexts.
      if(uptime<pendingAt_||uptime-pendingAt_>5000||memcmp(config_,edit.config().bytes+24,sizeof(config_))||memcmp(core_,service.config.bytes,sizeof(core_))){clear();return;}
      if(!idle(service))return;
      Service::ChannelScope channel(service,0);if(!channel.valid()){clear();return;}
      uint32_t id=0;const int result=service.submitModuleAnnouncement(pending_,pendingSize_,0,uint32_t(uptime),random,id,true);
      if(result==-EAGAIN)return; // no accepted packet yet; bounded by the5s deadline
      if(!result)sink(pending_,pendingSize_,id);
      clear();return;
    }
    Service::ChannelScope channel(service,0);if(!channel.valid())return;
    beacon::BeaconPayloadBatch batch{};
    if(!stock::MeshBeaconBroadcastModule::sendBeacon(cfg,batch,service.config.regionCode()))return;
    uint8_t first[239]{},second[239]{};size_t firstSize=0,secondSize=0;
    if(!encode(batch.packets[0],first,firstSize)||(batch.count==2&&!encode(batch.packets[1],second,secondSize)))return;
    if(persistWave(service,edit,utc,uint32_t(uptime))||!cadence_.attempted(uptime))return;
    uint32_t id=0;const int result=service.submitModuleAnnouncement(first,firstSize,0,uint32_t(uptime),random,id,true);
    if(result)return; // persisted history prevents a reboot/busy-loop resend wave
    sink(first,firstSize,id);
    if(batch.count==2){memcpy(pending_,second,secondSize);pendingSize_=secondSize;pendingAt_=uptime;memcpy(config_,edit.config().bytes+24,sizeof(config_));memcpy(core_,service.config.bytes,sizeof(core_));}
  }
};
}
