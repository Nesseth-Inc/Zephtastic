// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "CompletionConfig.h"
#include "audio/AudioModule.h"
#include "peripherals/ZephyrPeripherals.h"
#include "../telemetry/CompletionTelemetry.h"
#include "../stock/telemetry/SensorModules.h"
#include "../stock/telemetry/SensorCadence.h"
#if defined(CONFIG_MT_PMSA003I)
#include "../stock/telemetry/accessory/Accessories.h"
#endif
#include "../power/PowerPolicy.h"
#include "../power/ZephyrPowerBackend.h"
#include "../notifications/ExtendedNotificationController.h"
#include "../notifications/ZephyrExtendedNotification.h"
#include "../ModuleController.h"
#include <zephyr/sys/util.h>
namespace mt::modules {
// Main-loop-owned optional module composition. Settings are snapshotted under
// Service ownership, while device acquisition, codecs and GPIO playback run
// after releasing that lock. Neither callbacks nor hardware code route RF.
class CompletionRuntime {
  CompletionSettings pending_;ModuleSettings legacy_;NotificationSettings notification_;ExtendedModuleSettings extended_;
  bool ready_=false,appliedReady_=false,configured_=false,pendingChanged_=false;
  telemetry::CompletionTelemetry telemetry_;
  power::PowerPolicy power_;
  audio::ZephyrI2sAudio i2s_;
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  audio::AudioModule audio_;
  bool audioPermitted_ = false;
#endif
  peripherals::AmbientLightingCore ambient_;
#if IS_ENABLED(CONFIG_MT_PAXCOUNTER)
  peripherals::PaxcounterCore pax_;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  bool paxPermitted_=false, paxScanning_=false, paxRouter_=false;
#endif
#endif
  notification::ZephyrExtendedNotification notificationIo_;
  notification::ExtendedNotificationController notificationController_;
  bool dismissNotice_=false,localNotice_=false,noticeMuted_=false;
  stock::telemetry::SensorCadence airRf_,healthRf_;uint64_t sampleAt_=0;bool sampled_=false;
  struct Notice {uint32_t from=0,local=0,id=0,port=0,now=0;size_t size=0;bool muted=false,range=false;uint8_t payload[233]{};};
  Notice notices_[2]{};unsigned noticeHead_=0,noticeCount_=0;
  struct Seen {uint32_t from=0,id=0,at=0;};Seen seen_[8]{};unsigned nextSeen_=0;
  static bool newTag(unsigned tag){return tag==meshtastic_ModuleConfig_audio_tag||tag==meshtastic_ModuleConfig_ambient_lighting_tag||tag==meshtastic_ModuleConfig_paxcounter_tag;}
  bool get(meshtastic_ModuleConfig &out,unsigned tag)const{return completionConfig(pending_,legacy_,notification_,extended_,out,tag);}
  static bool encode(unsigned port,const uint8_t *payload,size_t length,uint8_t *out,size_t capacity,size_t &size){
    size=0;if(!payload||!length||length>233)return false;meshtastic_Data data{};data.portnum=static_cast<meshtastic_PortNum>(port);
    data.has_bitfield=true;data.payload.size=pb_size_t(length);memcpy(data.payload.bytes,payload,length);
    auto stream=pb_ostream_from_buffer(out,capacity);if(!pb_encode(&stream,meshtastic_Data_fields,&data))return false;size=stream.bytes_written;return true;
  }
public:
  static bool handles(unsigned tag){return newTag(tag)||tag==meshtastic_ModuleConfig_telemetry_tag||tag==meshtastic_ModuleConfig_external_notification_tag;}
  telemetry::CompletionTelemetry &telemetry(){return telemetry_;}
#if IS_ENABLED(CONFIG_MT_PAXCOUNTER)
  bool paxSnapshot(uint64_t now, meshtastic_Paxcount &out) {
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    if(!paxPermitted_||!paxScanning_)return false;
#endif
    return ready_ && configured_ && appliedReady_ && !pendingChanged_ &&
           pax_.reply(uint32_t(now), uint32_t(now / 1000), out);
  }
#endif
  bool noticeMuted()const{return noticeMuted_;}
  void muteNotices(bool muted){
    noticeMuted_=muted;
    if(muted){noticeCount_=0;localNotice_=false;dismissNotice_=true;}
  }
  void queueLocalNotification(){if(ready_&&!noticeMuted_)localNotice_=true;}
  bool requestNoticeDismissal(){
    return stock::notification::handleInputEvent(notificationController_.armed()&&!dismissNotice_,[this]{dismissNotice_=true;});
  }
  bool ringtone(const char *text,size_t size){return notificationController_.setRingtone(text,size);}
  bool powerMonitorEnabled()const{return ready_ && appliedReady_ && configured_ && !pendingChanged_ && power_.powerMonitorEnabled();}
  bool supportsPower(const meshtastic_Config_PowerConfig &c)const{return power_.configSupported(c);}
  // Bind owner hooks before accepting PowerConfig mutations. The owner must
  // signal notifyPowerActivity() from radio/button/BLE ingress paths and call
  // executePowerAction() only after releasing Service/Companion locks.
  bool bindPowerOwner(const power::PowerOwnerHooks &hooks){
    auto &backend=power::ZephyrPowerBackend::instance();
    if(!backend.configure(hooks))return false;
    power_.setDeepSleepBackend(backend.timedBackend());
    power_.setPowerOffBackend(backend.manualPowerOffBackend());
    return true;
  }
  void notifyPowerActivity(){power::ZephyrPowerBackend::notifyGlobalActivity();}
  void roleSleep(uint32_t role,uint32_t seconds,uint64_t now){power_.roleSleep(role,seconds,now);}
  power::PowerPolicy::State powerState()const{return power_.state();}
  power::PowerPolicy::DeepSleepResult executePowerAction(const power::PowerPolicy::Activity &activity){
    switch(power_.state()){
    case power::PowerPolicy::State::DeepSleepPending:return power_.executeDeepSleep(activity);
    case power::PowerPolicy::State::BatteryShutdownPending:return power_.executeBatteryShutdown(activity);
    default:return power::PowerPolicy::DeepSleepResult::NotEligible;
    }
  }
  power::PowerPolicy::DeepSleepResult executeRequestedPowerOff(const power::PowerPolicy::Activity &activity,bool explicitConsent=false){
    return power_.executeRequestedPowerOff(activity,explicitConsent);
  }
  bool supportsRequestedPowerOff()const{return power_.supportsRequestedPowerOff();}
  float adcMultiplierOverride()const{return power_.adcMultiplierOverride();}
  // Return the explicitly selected INA address from the staged completion
  // snapshot. Disabling policy readiness does not silently change the
  // configured battery-source selection.
  uint32_t inaBatteryAddress()const{
    meshtastic_Config_PowerConfig config{};
    return pending_.power(config)?config.device_battery_ina_address:0;
  }
  bool supports(const meshtastic_ModuleConfig &wire)const{
    switch(wire.which_payload_variant){
    case meshtastic_ModuleConfig_audio_tag:{audio::AudioConfig c;if(!audio::AudioConfig::fromWire(wire,c))return false;
      const auto caps=audio::AudioModule::availableCapabilities(c,i2s_);
      return !c.enabled||(IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)&&caps.codec2&&caps.capture&&caps.playback&&caps.ptt);}
    case meshtastic_ModuleConfig_ambient_lighting_tag:{const auto &c=wire.payload_variant.ambient_lighting;
      return c.current<=255&&c.red<=255&&c.green<=255&&c.blue<=255&&(!c.led_state||peripherals::zephyr::ambientAvailable());}
    case meshtastic_ModuleConfig_paxcounter_tag:{const auto &c=wire.payload_variant.paxcounter;
      return c.paxcounter_update_interval<=INT32_MAX/1000u&&c.wifi_threshold<=0&&c.wifi_threshold>=-127&&c.ble_threshold<=0&&c.ble_threshold>=-127&&
        (!c.enabled||IS_ENABLED(CONFIG_MT_PAXCOUNTER));}
    case meshtastic_ModuleConfig_telemetry_tag:return telemetry_.configSupported(wire.payload_variant.telemetry);
    case meshtastic_ModuleConfig_external_notification_tag:{notification::ExtendedNotificationConfig c;if(!notification::ExtendedNotificationConfig::fromWire(wire,c))return false;
      const auto caps=notification::ZephyrExtendedNotification::availableCapabilities(c,{false,true,true,true,true});
      return !c.enabled||((!c.alertMessageVibra&&!c.alertBellVibra||caps.vibrator)&&
        ((!c.alertMessageBuzzer&&!c.alertBellBuzzer)||(c.usePwm?caps.pwmBuzzer:c.useI2s?caps.i2sRtttl:caps.gpioBuzzer))&&
        (!c.usePwm||caps.pwmBuzzer)&&(!c.useI2s||caps.i2sRtttl));}
    default:return false;
    }
  }
  uint32_t availableModules()const{
    uint32_t bits=0;meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_audio_tag;c.payload_variant.audio.codec2_enabled=true;
    if(supports(c))bits|=meshtastic_ExcludedModules_AUDIO_CONFIG;
    if(peripherals::zephyr::ambientAvailable())bits|=meshtastic_ExcludedModules_AMBIENTLIGHTING_CONFIG;
    if(IS_ENABLED(CONFIG_MT_PAXCOUNTER))bits|=meshtastic_ExcludedModules_PAXCOUNTER_CONFIG;
    c={};c.which_payload_variant=meshtastic_ModuleConfig_external_notification_tag;c.payload_variant.external_notification.enabled=true;
    const auto caps=notification::ZephyrExtendedNotification::availableCapabilities(c,{false,true,true,true,true});
    if(caps.vibrator||caps.gpioBuzzer||caps.pwmBuzzer||caps.i2sRtttl)bits|=meshtastic_ExcludedModules_EXTNOTIF_CONFIG;
    return bits;
  }
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  void paxPermitted(bool ready,bool bluetoothEnabled,bool wifiEnabled,bool router) {
    const bool allowed=ready&&stock::paxcounter::Interval::permitted(bluetoothEnabled,wifiEnabled);
    if(paxPermitted_!=allowed||paxRouter_!=router){paxPermitted_=allowed;paxRouter_=router;pendingChanged_=true;}
  }
#endif
  void stage(const CompletionSettings &settings,const ModuleSettings &legacy,const NotificationSettings &notification,
             const ExtendedModuleSettings &extended,bool ready,uint32_t regionCode){
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
    const bool permitted = audio::AudioModule::regionPermits(regionCode);
    if (audioPermitted_ != permitted) pendingChanged_ = true;
    audioPermitted_ = permitted;
#else
    (void)regionCode;
#endif
    const bool changed=!pending_.same(settings)||!legacy_.same(legacy)||!notification_.same(notification)||
      !extended_.same(extended)||ready_!=ready;
    if(changed){pending_=settings;legacy_=legacy;notification_=notification;extended_=extended;ready_=ready;pendingChanged_=true;}
  }
  void hardwarePoll(uint64_t now,const power::PowerPolicy::Activity &activity){
    const bool changed=!configured_||pendingChanged_||appliedReady_!=ready_;
    if(changed){
      meshtastic_ModuleConfig c{};
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
      get(c,meshtastic_ModuleConfig_audio_tag);audio::AudioConfig a{};(void)audio::AudioConfig::fromWire(c,a);a.enabled=a.enabled&&ready_&&audioPermitted_;(void)audio_.configure(a,i2s_);
#endif
      get(c,meshtastic_ModuleConfig_ambient_lighting_tag);const auto &l=c.payload_variant.ambient_lighting;
      ambient_.configure({ready_&&l.led_state,uint8_t(l.current),uint8_t(l.red),uint8_t(l.green),uint8_t(l.blue)});
#if IS_ENABLED(CONFIG_MT_PAXCOUNTER)
      get(c,meshtastic_ModuleConfig_paxcounter_tag);const auto &p=c.payload_variant.paxcounter;
      bool enabled=ready_&&p.enabled;
      uint32_t interval=p.paxcounter_update_interval;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
      enabled=enabled&&paxPermitted_;
      interval=stock::paxcounter::Interval::reportPeriod(interval,paxRouter_);
#endif
      pax_.configure({enabled,interval,p.wifi_threshold,p.ble_threshold},uint32_t(now));
#endif
      get(c,meshtastic_ModuleConfig_external_notification_tag);notification::ExtendedNotificationConfig n{};(void)notification::ExtendedNotificationConfig::fromWire(c,n);
      n.enabled=ready_&&n.enabled;n.alertMessage=false;n.alertBell=false; // LED remains owned by the established controller.
      // Audio and notification playback share one physical I2S owner.
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
      if(a.enabled)n.useI2s=false;
#endif
      auto caps=notificationIo_.configure(n,&i2s_);(void)notificationController_.configure(n,caps,notificationIo_.backend());
      get(c,meshtastic_ModuleConfig_telemetry_tag);auto t=c.payload_variant.telemetry;
      if(!ready_){t.air_quality_enabled=false;t.health_measurement_enabled=false;}
      (void)telemetry_.configure(t);
      airRf_.configure(t.air_quality_enabled,t.air_quality_interval,now,85000);
#if defined(CONFIG_MT_PMSA003I)
      stock::telemetry::accessory::airPollDeadline(airRf_.nextPoll());
#endif
      healthRf_.configure(t.health_measurement_enabled,t.health_update_interval,now,100000);
      meshtastic_Config_PowerConfig power{};(void)pending_.power(power);if(!ready_)power.is_power_saving=false;(void)power_.configure(power,now);
      appliedReady_=ready_;configured_=true;pendingChanged_=false;sampled_=false;
    }
#if IS_ENABLED(CONFIG_MT_PAXCOUNTER)
    if(pax_.enabled()){
      (void)peripherals::zephyr::startBleObserver(true);
      peripherals::zephyr::startWifiObserver(true);
    }else{peripherals::zephyr::stopBleObserver();peripherals::zephyr::stopWifiObserver();}
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    paxScanning_=pax_.enabled()&&peripherals::zephyr::bleObservationAvailable()&&peripherals::zephyr::wifiObservationAvailable();
    peripherals::zephyr::capturePax(paxScanning_);
#endif
    (void)peripherals::zephyr::drainPaxObservations(pax_);
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    // Drain older queued samples before publishing the completed report window.
    if(paxScanning_)pax_.publishIntervals(uint32_t(now));
#endif
#endif
    (void)ambient_.poll(uint32_t(now),peripherals::zephyr::ambientAvailable(),nullptr,peripherals::zephyr::applyAmbient,peripherals::zephyr::ambientOff);
    if(!sampled_||now-sampleAt_>=1000){(void)telemetry_.poll(now);sampleAt_=now;sampled_=true;}
    if(localNotice_){notificationController_.startNotification(uint32_t(now));localNotice_=false;}
    if(dismissNotice_){notificationController_.stop();dismissNotice_=false;}
    while(noticeCount_){const auto &n=notices_[noticeHead_];(void)notificationController_.receive(n.from,n.local,n.id,n.port,n.payload,n.size,n.muted,n.range,n.now);noticeHead_=(noticeHead_+1)%2;--noticeCount_;}
    notificationController_.poll(uint32_t(now));
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
    audio_.poll(uint32_t(now));
#endif
    (void)power_.poll(now,activity);
    // Policy only announces actions here. The main owner executes them after
    // dropping Service ownership and supplies final activity/quiescence state.
  }
  bool response(uint8_t variant,uint32_t request,uint64_t now,uint32_t utc,uint8_t *out,size_t capacity,size_t &size)const{
    size=0;return ready_&&telemetry_.response(variant,request,now,utc,out,capacity,size)==telemetry::CompletionTelemetry::Result::Ok;
  }
  void receive(uint32_t from,uint32_t to,uint32_t local,uint32_t id,unsigned port,unsigned channel,const uint8_t *payload,size_t size,bool muted,bool range,uint64_t now){
    if(!ready_||!id||!payload||size>233||(to!=UINT32_MAX&&to!=local)||from==local)return;
    for(const auto &s:seen_)if(s.from==from&&s.id==id&&uint32_t(now-s.at)<600000)return;
    seen_[nextSeen_++%8]={from,id,uint32_t(now)};
    if(port==meshtastic_PortNum_TELEMETRY_APP)(void)telemetry_.observePeer(from,payload,size,now);
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
    if(audioPermitted_&&!muted)(void)audio_.receive(from,channel,port,payload,size);
#endif
    if(!noticeMuted_&&noticeCount_<2){auto &n=notices_[(noticeHead_+noticeCount_)%2];n={};n.from=from;n.local=local;n.id=id;n.port=port;n.now=uint32_t(now);n.size=size;n.muted=muted;n.range=range;memcpy(n.payload,payload,size);++noticeCount_;}
  }
  template<class Sink>void ownerPoll(Service &service,ModuleController &modules,uint64_t now,uint32_t utc,uint32_t random,Sink sink){
    if(!ready_)return;uint8_t payload[233]{},data[239]{};size_t size=0;uint32_t id=0;
#if IS_ENABLED(CONFIG_ZEPHCORE_AUDIO_CODEC2)
    if(audio::AudioModule::regionPermits(service.config.regionCode()))
      audio_.sendPayload(payload,sizeof(payload),[&](uint32_t audioPort,const uint8_t *body,size_t bytes) {
        if(!encode(audioPort,body,bytes,data,sizeof(data),size) ||
           service.submitModuleAnnouncement(data,size,service.config.hopLimit(),uint32_t(now),random,id) || !id)return false;
        sink(data,size,id);return true;
      });
#endif
#if IS_ENABLED(CONFIG_MT_PAXCOUNTER)
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    if(paxPermitted_&&paxScanning_)
#endif
    pax_.sendInfo(uint32_t(now), uint32_t(now / 1000), [&](const meshtastic_Paxcount &count) {
      auto stream = pb_ostream_from_buffer(payload, sizeof(payload));
      if (!pb_encode(&stream, meshtastic_Paxcount_fields, &count) ||
          !encode(meshtastic_PortNum_PAXCOUNTER_APP, payload, stream.bytes_written, data, sizeof(data), size)) return false;
      id = 0;
      if (service.submitModuleAnnouncement(data, size, service.config.hopLimit(), uint32_t(now), random ^ 0x706178u, id) || !id)
        return false;
      sink(data, size, id);
      return true;
    });
#endif
    telemetry::CompletionTelemetry::Snapshot snapshot{};(void)telemetry_.snapshot(now,snapshot);
    constexpr unsigned telemetryVariants[]={
      unsigned(meshtastic_Telemetry_air_quality_metrics_tag),
      unsigned(meshtastic_Telemetry_health_metrics_tag),
    };
    for(unsigned variant:telemetryVariants){
      const bool air=variant==meshtastic_Telemetry_air_quality_metrics_tag;
      auto &schedule=air?airRf_:healthRf_;
      if(!schedule.beginPoll(now))continue;
#if defined(CONFIG_MT_PMSA003I)
      if(air) stock::telemetry::accessory::airPollDeadline(schedule.nextPoll());
#endif
      uint32_t interval=0;
      const auto kind=air?history::Kind::Air:history::Kind::Health;
      const bool due=modules.sensorInterval(service,schedule.configuredSeconds(),now,air,interval) &&
                     schedule.meshDue(now,interval)&&service.broadcastDue(kind,interval);
      bool rf=false;
      if(due && !service.moduleAnnouncementPending()) {
        const auto channel=service.channelUtilization();
        rf=channel && TxAdmission::channel(service.channelMeter().diagnosticSupported(),*channel,service.config.role()!=DeviceRolePolicy::Role::SENSOR)==TxAdmission::Decision::Allow
           && service.moduleAirtimeAllowed();
      }
      if(rf&&service.config.role()==DeviceRolePolicy::Role::SENSOR)
        modules.noteRoleSleep(schedule.configuredSeconds());
      // Health stamps an eligible mesh opportunity even when the read fails.
      // AirQuality stamps after the measurement packet is accepted.
      if(rf&&!air)service.broadcastAdmitted(kind);
      if(air?!snapshot.hasAir:!snapshot.hasHealth)continue;
      meshtastic_Telemetry t{};
      if (!stock::telemetry::allocReply(variant, snapshot, utc, t)) continue;
      auto stream=pb_ostream_from_buffer(payload,sizeof(payload));if(!pb_encode(&stream,meshtastic_Telemetry_fields,&t)||!encode(meshtastic_PortNum_TELEMETRY_APP,payload,stream.bytes_written,data,sizeof(data),size))continue;
      id=0;if(rf)(void)service.submitModuleAnnouncement(data,size,service.config.hopLimit(),uint32_t(now),random^variant,id);
      if(id){schedule.queued(now);if(air)service.broadcastAdmitted(kind);}
      sink(data,size,id?id:random^variant,!id);
    }
  }
};
} // namespace mt::modules
