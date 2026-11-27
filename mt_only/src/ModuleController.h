// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Service.h"
#include "ModuleSettings.h"
#include "ModuleSchedule.h"
#include "ModuleIntervals.h"
#include "NeighborIntervals.h"
#include "OnlineObservations.h"
#include "stock/modules/NeighborInfoModule.h"
#include "StatusMessagePayload.h"
#include "TelemetryAnnouncement.h"
#include "EnvironmentAnnouncement.h"
#include "PowerAnnouncement.h"
#include "RangeTestPayload.h"
namespace mt {
// One application-owned composition. No dynamic allocation, catch-up loop,
// client owner slot, wall-clock adoption or invented sensor reading.
class ModuleController {
  ModuleSettings settings_;bool configured_=false,router_=false;
  ModuleSchedule devicePhone_,statsPhone_,deviceRf_,status_,neighbor_,range_;
  ModuleSchedule environmentPoll_,environmentPhone_,powerPoll_,powerPhone_;
  NeighborObservations neighbors_;OnlineObservations online_;uint64_t deviceEligibleAt_=0;
  stock::RangeTestModule rangeTest_;
  bool deviceHistory_=false;
  uint64_t environmentEligibleAt_=0;bool environmentHistory_=false;
#if defined(CONFIG_MT_LIGHTNING_EVENTS)
  bool strikeAccepted_=false;
  uint64_t acceptedStrikeMs_=0,nextStrikeCheck_=0;
#endif
  uint64_t powerEligibleAt_=0;bool powerHistory_=false;
  uint32_t localSequence_=0,roleSleepSeconds_=0;
  static uint32_t interval(const uint8_t*p,uint32_t fallback){const auto value=Config::get32(p);return (value?value:fallback)*1000;}
  void configure(const ModuleSettings&settings,uint64_t uptime,bool router){
    const uint32_t now=uint32_t(uptime);
    if(!configured_){devicePhone_.configure(now,60000,60000);statsPhone_.configure(now,900000,60000);}
    if(!configured_||memcmp(settings_.bytes+4,settings.bytes+4,5)){
      // The single ZT telemetry registration uses stock index0: first30s,
      // then minute checks. Interval edits do not erase branch history.
      deviceRf_.disable();if(settings.bytes[4])deviceRf_.configure(now,60000,30000);
    }
    if(!configured_||settings_.environmentEnabled()!=settings.environmentEnabled()||
       settings_.environmentInterval()!=settings.environmentInterval()){
      environmentPoll_.disable();environmentPhone_.disable();
      if(settings.environmentEnabled()){
        // Existing device telemetry is registration0. Environment starts its
        // first10s init phase plus the next30+15s stagger; no catch-up burst.
        environmentPoll_.configure(now,1000,55000);environmentPhone_.configure(now,60000,55000);
      }
    }
    if(!configured_||settings_.powerEnabled()!=settings.powerEnabled()||
       settings_.powerInterval()!=settings.powerInterval()){
      powerPoll_.disable();powerPhone_.disable();
      // Shared producer has one init. This third local registration starts
      // after the environment phase, without a catch-up loop.
      if(settings.powerEnabled()){powerPoll_.configure(now,1000,70000);powerPhone_.configure(now,60000,70000);}
    }
    if(!configured_||memcmp(settings_.bytes+20,settings.bytes+20,80)){
      status_.disable();if(settings.bytes[20])status_.configure(now,stock::StatusMessageModule::IntervalMs,stock::StatusMessageModule::FirstDelayMs);
    }
    if(!configured_||router_!=router||memcmp(settings_.bytes+9,settings.bytes+9,6)){
      neighbor_.disable();
      if(settings.bytes[9])neighbor_.configure(now,NeighborIntervals::steadyMs(Config::get32(settings.bytes+10)),NeighborIntervals::initialMs(Config::get32(settings.bytes+10),router));
      else neighbors_.clear();
    }
    if(!configured_||memcmp(settings_.bytes+15,settings.bytes+15,5)){
      range_.disable();if(settings.bytes[15]&&Config::get32(settings.bytes+16))range_.configure(now,interval(settings.bytes+16,0),stock::RangeTestModule::FirstDelayMs);
      rangeTest_.start(uptime);
    }
    settings_=settings;router_=router;configured_=true;
  }
  enum class IntervalKind {Position, Telemetry, CongestionOnly};
  bool scaledInterval(Service&service,uint32_t configuredSeconds,uint64_t uptime,IntervalKind kind,uint32_t&out){
    const auto *region=radio::RegionPolicy::lookup(service.config.regionCode());
    if(!region||!radio::RegionPolicy::softwareUsableOnSx126x(*region,false,false))return false;
    const auto role=service.config.role();if(!DeviceRolePolicy::supportedRole(role))return false;
    // Default::getConfiguredOrDefaultMsScaled uses the same role exceptions for all traffic.
    const bool router=role==DeviceRolePolicy::Role::ROUTER||role==DeviceRolePolicy::Role::ROUTER_LATE;
    const int8_t throttle=kind==IntervalKind::Position?region->profile->positionThrottle:
        (kind==IntervalKind::CongestionOnly?1:region->profile->telemetryThrottle);
    if(role==DeviceRolePolicy::Role::SENSOR || role==DeviceRolePolicy::Role::TRACKER ||
       role==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER){
      out=ModuleIntervals::throttle(ModuleIntervals::base(configuredSeconds,false),throttle);
      return true;
    }
    uint32_t count=0;unsigned sf=0;float bw=0;
    if(!router){
      if(!online_.count(service.nodes,service.route.identity(),uptime,count))return false;
      LoRaPresets::Profile profile{};USChannelSelection selection{};
      if(!service.config.resolveRadio(profile,selection))return false;
      sf=profile.spreadingFactor;bw=float(profile.bandwidthHz)/1000.f;
    }
    return ModuleIntervals::device(configuredSeconds,router,true,count,sf,bw,false,throttle,out);
  }
  bool deviceInterval(Service&service,uint64_t uptime,uint32_t&out){return telemetryInterval(service,Config::get32(settings_.bytes+5),uptime,out);}
  bool environmentInterval(Service&service,uint64_t uptime,uint32_t&out){
    return telemetryInterval(service,settings_.environmentInterval(),uptime,out);
  }
  static bool data(meshtastic_PortNum port,const uint8_t*payload,size_t length,uint8_t*out,size_t&size){
    size=0;if(!payload||!length||length>233)return false;
    meshtastic_Data d{};d.portnum=port;d.has_bitfield=true;d.payload.size=length;memcpy(d.payload.bytes,payload,length);
    auto stream=pb_ostream_from_buffer(out,239);if(!pb_encode(&stream,meshtastic_Data_fields,&d))return false;
    size=stream.bytes_written;return true;
  }
  uint32_t localId(){if(!++localSequence_)++localSequence_;return localSequence_;}
public:
  void noteRoleSleep(uint32_t seconds){
    const auto duration=ModuleIntervals::base(seconds,false)/1000;
    if(!roleSleepSeconds_||duration<roleSleepSeconds_)roleSleepSeconds_=duration;
  }
  uint32_t takeRoleSleep(){const auto duration=roleSleepSeconds_;roleSleepSeconds_=0;return duration;}
  bool nodeInfoInterval(Service &service,uint64_t uptime,uint32_t &out){
    return scaledInterval(service,1800,uptime,IntervalKind::CongestionOnly,out);
  }
  bool telemetryInterval(Service&service,uint32_t seconds,uint64_t uptime,uint32_t&out){
    return scaledInterval(service,seconds,uptime,IntervalKind::Telemetry,out);
  }
  // AirQualityTelemetry uses Default's three-argument scale. HealthTelemetry
  // requests the TELEMETRY traffic factor in addition to congestion scaling.
  bool sensorInterval(Service& service,uint32_t seconds,uint64_t uptime,bool air,uint32_t& out){
    return scaledInterval(service,seconds,uptime,air?IntervalKind::CongestionOnly:IntervalKind::Telemetry,out);
  }
  // Position and telemetry share stock scaling, with their own profile factors.
  // Position-specific stationary/smart policy remains with its caller.
  bool positionInterval(Service&service,uint32_t seconds,uint64_t uptime,uint32_t&out){return scaledInterval(service,seconds,uptime,IntervalKind::Position,out);}
  void observe(uint32_t id,uint32_t self,unsigned hops,uint64_t uptime,bool known,float snr){
    if(hops<=7)online_.observe(id,self,uptime);
    if(configured_&&settings_.bytes[9])
      stock::NeighborInfoModule::handleReceivedProtobuf(neighbors_,id,self,hops,uptime,known,snr,
        Config::get32(settings_.bytes+10),nullptr);
  }
  void observeNeighborInfo(Service&,uint32_t source,uint32_t self,unsigned hops,uint64_t uptime,
                           bool known,float snr,const meshtastic_NeighborInfo&info){
    if(hops<=7)online_.observe(source,self,uptime);
    if(!configured_||!settings_.bytes[9]||hops>7)return;
    stock::NeighborInfoModule::handleReceivedProtobuf(neighbors_,source,self,hops,uptime,known,snr,
      Config::get32(settings_.bytes+10),&info);
  }
  int localNeighbors(Service&service,const ModuleSettings&settings,bool ready,uint64_t uptime,meshtastic_NeighborInfo&out){
    out={};
    if(!ready||!settings.valid()||!settings.bytes[9]||!online_.advance(uptime))return -EACCES;
    const uint32_t seconds=NeighborIntervals::advertisedSeconds(Config::get32(settings.bytes+10),service.config.role()==2);
    // snapshot may prune expired observations, but does not configure/advance
    // schedules, reserve IDs, write settings or send RF. Empty is available.
    (void)neighbors_.snapshot(service.nodes,service.route.identity(),seconds,uptime,out);
    return out.node_id==service.route.identity()?0:-EACCES;
  }
  // Sink is a local connected-client copy, never evidence of RF delivery.
  template<class Sink>void poll(Service&service,const ModuleSettings&settings,bool ready,uint64_t uptime,uint32_t random,Sink sink){
    pollTimed(service,settings,ready,uptime,random,0,
      [&](const uint8_t *p,size_t n,uint32_t id,bool){sink(p,n,id);});
  }
  enum class PhoneDelivery {Normal,Idle,FollowUp};
  static constexpr uint8_t AllPhones=0x0f;
  template<class Sink>void pollTimed(Service&service,const ModuleSettings&settings,bool ready,uint64_t uptime,uint32_t random,uint32_t utc,Sink sink){
    pollReports(service,settings,ready,uptime,random,utc,
      [&](const uint8_t *p,size_t n,uint32_t id,PhoneDelivery delivery,uint8_t recipients){
        sink(p,n,id,delivery!=PhoneDelivery::Normal);return recipients;
      });
  }
  // The return mask identifies transports that actually accepted this packet.
  template<class Send>void pollReports(Service&service,const ModuleSettings&settings,bool ready,uint64_t uptime,uint32_t random,uint32_t utc,Send send){
    auto sink=[&](const uint8_t *p,size_t n,uint32_t id,bool idle){
      return send(p,n,id,idle?PhoneDelivery::Idle:PhoneDelivery::Normal,AllPhones);
    };
    if(!ready||!settings.valid()||uptime==UINT64_MAX||uptime/1000>UINT32_MAX||!online_.advance(uptime))return;
    const bool router=service.config.role()==2;
    if(!configured_||router_!=router||!settings_.same(settings))configure(settings,uptime,router);
    const uint32_t now=uint32_t(uptime),seconds=uint32_t(uptime/1000);
    uint8_t bytes[239]{};size_t size=0;
    const bool phone=devicePhone_.due(now),rf=deviceRf_.due(now);
    if(phone||rf){
      if(phone)devicePhone_.consumed(now);
      if(rf)deviceRf_.consumed(now); // Check cadence, not mesh history.
      const auto air=service.airUtilTx(),channel=service.channelUtilization();
      uint32_t meshInterval=0;
      const bool eligible=rf&&deviceInterval(service,uptime,meshInterval)&&
        (!deviceHistory_||uptime-deviceEligibleAt_>=meshInterval)&&
        service.broadcastDue(history::Kind::Device,meshInterval)&&
        service.channelMeter().diagnosticSupported()&&channel&&
        stock::telemetry::deviceMeshAllowed(service.config.role(),*channel,service.moduleAirtimeAllowed());
      // Stock records the eligible branch after sendTelemetry regardless of
      // its bool result. Service queue/nonce/admission still decides delivery.
      if(eligible){deviceHistory_=true;deviceEligibleAt_=uptime;service.broadcastAdmitted(history::Kind::Device);}
      if(TelemetryAnnouncement::encode(seconds,air,channel,bytes,sizeof(bytes),size,service.hardware(),utc)){
        uint32_t id=0;
        if(eligible)(void)service.submitModuleAnnouncement(bytes,size,service.config.hopLimit(),now,random,id);
        if(id)sink(bytes,size,id,false);
        else if(phone&&!eligible){
          const uint8_t recipients=sink(bytes,size,localId(),true);
          // Stock admits DeviceMetrics and LocalStats together on an idle phone.
          // Append stats only to sessions that accepted the preceding device report.
          if(recipients&&statsPhone_.due(now)&&LocalStatsQuery::announcement(seconds,air,channel,
              bytes,sizeof(bytes),size,service.localStats(),utc)&&send(bytes,size,localId(),PhoneDelivery::FollowUp,recipients))
            statsPhone_.consumed(now);
        }
      }
    }
    bool strikeDue=false;
#if defined(CONFIG_MT_LIGHTNING_EVENTS)
    const auto &strike=service.environment().accessory;
    strikeDue=settings_.environmentEnabled()&&strike.hasStrikes&&strike.strikes&&strike.hasLastStrike&&
      uptime>=strike.lastStrikeMs&&uptime-strike.lastStrikeMs<300000&&
      (!strikeAccepted_||acceptedStrikeMs_!=strike.lastStrikeMs);
#endif
    const bool scheduledEnvironment=environmentPoll_.due(now);
    bool environmentDue=scheduledEnvironment;
#if defined(CONFIG_MT_LIGHTNING_EVENTS)
    environmentDue|=strikeDue&&uptime>=nextStrikeCheck_;
#endif
    if(environmentDue){
      if(scheduledEnvironment)environmentPoll_.consumed(now);
      const bool phone=environmentPhone_.due(now);
      if(phone)environmentPhone_.consumed(now);
      uint32_t meshInterval=0;const auto channel=service.channelUtilization();
      const bool eligible=environmentInterval(service,uptime,meshInterval)&&
        (!environmentHistory_||strikeDue||uptime-environmentEligibleAt_>=meshInterval)&&
        (strikeDue||service.broadcastDue(history::Kind::Environment,meshInterval))&&
        service.channelMeter().diagnosticSupported()&&channel&&
        stock::telemetry::environmentMeshAllowed(service.config.role(),*channel,service.moduleAirtimeAllowed());
#if defined(CONFIG_MT_LIGHTNING_EVENTS)
      if(strikeDue)nextStrikeCheck_=uptime+1000;
#endif
      if(eligible&&!strikeDue){
        environmentHistory_=true;environmentEligibleAt_=uptime;service.broadcastAdmitted(history::Kind::Environment);
        if(service.config.role()==DeviceRolePolicy::Role::SENSOR)noteRoleSleep(settings_.environmentInterval());
      }
      if((eligible||phone)&&EnvironmentAnnouncement::encode(service.environment(),bytes,sizeof(bytes),size,utc)){
        uint32_t id=0;if(eligible)(void)service.submitModuleAnnouncement(bytes,size,service.config.hopLimit(),now,random^0x656eu,id);
#if defined(CONFIG_MT_LIGHTNING_EVENTS)
        if(id&&strikeDue){
          strikeAccepted_=true;acceptedStrikeMs_=strike.lastStrikeMs;
          environmentHistory_=true;environmentEligibleAt_=uptime;service.broadcastAdmitted(history::Kind::Environment);
          if(service.config.role()==DeviceRolePolicy::Role::SENSOR)noteRoleSleep(settings_.environmentInterval());
        }
#endif
        if(phone||id)sink(bytes,size,id?id:localId(),!id);
      }
    }
    if(powerPoll_.due(now)){
      powerPoll_.consumed(now);const bool phone=powerPhone_.due(now);
      if(phone)powerPhone_.consumed(now);
      uint32_t meshInterval=0;
      const bool eligible=telemetryInterval(service,settings_.powerInterval(),uptime,meshInterval)&&
        (!powerHistory_||uptime-powerEligibleAt_>=meshInterval)&&
        service.broadcastDue(history::Kind::Power,meshInterval)&&
        service.moduleAirtimeAllowed();
      if(eligible){
        powerHistory_=true;powerEligibleAt_=uptime;service.broadcastAdmitted(history::Kind::Power);
        if(service.config.role()==DeviceRolePolicy::Role::SENSOR)noteRoleSleep(settings_.powerInterval());
      }
      if((eligible||phone)&&PowerAnnouncement::encode(service.power(),bytes,sizeof(bytes),size,utc)){
        uint32_t id=0;if(eligible)(void)service.submitModuleAnnouncement(bytes,size,service.config.hopLimit(),now,random^0x7077u,id);
        if(phone||id)sink(bytes,size,id?id:localId(),!id);
      }
    }
    if(status_.due(now)){
      char nodeStatus[80];
      memcpy(nodeStatus,settings.bytes+20,sizeof(nodeStatus));
      auto send=[&](const meshtastic_Data &packet) {
        auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
        if(!pb_encode(&stream,meshtastic_Data_fields,&packet))return;
        uint32_t id=0;
        {
          Service::ChannelScope primary(service,0);
          if(primary.valid())
            (void)service.submitModuleAnnouncement(bytes,stream.bytes_written,service.config.hopLimit(),now,random^0x7341u,id);
        }
        sink(bytes,stream.bytes_written,id?id:localId(),false);
      };
      const auto next=stock::StatusMessageModule::runOnce(nodeStatus,stock::StatusMessageSender(send));
      status_.configure(now,next,next);
    }
    auto politeChannelAllowed=[&]() {
      const auto channel=service.channelUtilization();
      return service.channelMeter().diagnosticSupported() && channel &&
             TxAdmission::channel(true,*channel,true)==TxAdmission::Decision::Allow;
    };
    if(neighbor_.due(now)){
      const uint32_t configured=Config::get32(settings.bytes+10);
      const uint32_t interval=NeighborIntervals::advertisedSeconds(configured,router);
      const auto next=stock::NeighborInfoModule::runOnce(configured,settings.bytes[14],
          !PrimaryChannel::defaultKey(service.config.node()),politeChannelAllowed,
          [&](){return service.moduleAirtimeAllowed();},[&](bool overLoRa) {
            stock::NeighborInfoModule::sendNeighborInfo(neighbors_,service.nodes,service.route.identity(),interval,uptime,
                [&](const uint8_t *payload,size_t length) {
                  if(!data(meshtastic_PortNum_NEIGHBORINFO_APP,payload,length,bytes,size))return;
                  uint32_t id=0;
                  if(overLoRa)(void)service.submitModuleAnnouncement(bytes,size,0,now,random^0x6e69u,id);
                  sink(bytes,size,id?id:localId(),false);
                });
          });
      neighbor_.configure(now,next,next);
    }
    if(range_.due(now)){
      range_.consumed(now);
      const bool active=rangeTest_.runOnce(uptime,politeChannelAllowed(),[&](const uint8_t *payload,size_t length) {
        if(!data(meshtastic_PortNum_RANGE_TEST_APP,payload,length,bytes,size))return false;
        uint32_t id=0;
        (void)service.submitModuleAnnouncement(bytes,size,0,now,random^0x7267u,id);
        sink(bytes,size,id?id:localId(),false);
        return true;
      });
      if(!active)range_.disable();
    }
  }
};
}
