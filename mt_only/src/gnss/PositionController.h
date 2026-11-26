// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PositionSchedule.h"
#include "../stock/position/LostAndFound.h"
#include "PositionDistance.h"
#include "../PositionPrecision.h"
#include "../stock/position/PositionChannels.h"
#include "../ModuleController.h"

namespace mt {
// Serialized main-loop caller. Existing Service keeps location consent,
// current-fix validity, endpoint, RF, airtime and nonce/identity admission.
class PositionController {
  PositionSchedule schedule_;
  uint64_t lastPeriodicCheck_=0;
  uint64_t observedUptime_=0;
  int32_t observedLatitude_=0,observedLongitude_=0;
  bool checked_=false;
  bool observed_=false;
  bool observedFix_=false;
public:
  void restoreLastSend(uint32_t stamp){schedule_.restoreLastSend(stamp);}
  const PositionSchedule &schedule()const{return schedule_;}
  void poll(Service &service,ModuleController &modules,bool ready,uint64_t uptime,uint32_t random){
    if(uptime==UINT64_MAX||(observed_&&uptime<observedUptime_))return;
    observed_=true;observedUptime_=uptime;
    const auto g=service.config.gnssSettings();
    if(!ready||!(g.enabled||g.broadcastSecs||g.smart)||!service.endpointArmed())return;
    PositionChannels channels(service.config);
    uint8_t channelIndex=0;
    if(!stock::findPositionChannel(channels,channelIndex))return;
    const unsigned precision=service.config.positionPrecision(channelIndex);
    meshtastic_Position p{};
    if(!service.localPosition(p)||!p.has_latitude_i||!p.has_longitude_i||
       p.latitude_i<-900000000||p.latitude_i>900000000||p.longitude_i<-1800000000||p.longitude_i>1800000000)return;
    const auto channel=service.channelUtilization();
    const auto role=service.config.role();
    const bool tracker=role==meshtastic_Config_DeviceConfig_Role_TRACKER||
      role==meshtastic_Config_DeviceConfig_Role_TAK_TRACKER;
    if(!service.channelMeter().diagnosticSupported()||!channel||
       TxAdmission::channel(true,*channel,!tracker)!=TxAdmission::Decision::Allow)return;
    PositionSchedule::Parameters policy;
    if(!modules.positionInterval(service,g.broadcastSecs,uptime,policy.regularIntervalMs))return;
    policy.smartEnabled=g.smart;
    policy.smartMinimumIntervalMs=PositionSchedule::secondsToMs(g.smartMinInterval,300);
    policy.smartMinimumDistanceM=PositionSchedule::distanceThreshold(g.smartMinDistance);
    const int32_t oldLat=PositionPrecision::truncateCoordinate(schedule_.lastLatitude(),precision);
    const int32_t oldLon=PositionPrecision::truncateCoordinate(schedule_.lastLongitude(),precision);
    const int32_t lat=PositionPrecision::truncateCoordinate(p.latitude_i,precision);
    const int32_t lon=PositionPrecision::truncateCoordinate(p.longitude_i,precision);
    const unsigned movementPrecision=tracker ?
        stock::getPositionPrecisionForChannel(channels.getByIndex(channelIndex)) : precision;
    policy.stationary=service.config.fixedPosition()||
      (role!=DeviceRolePolicy::Role::LOST_AND_FOUND && (schedule_.lastLatitude() || schedule_.lastLongitude()) &&
       stock::position::positionWithinPrecisionCell(p.latitude_i,p.longitude_i,
         schedule_.lastLatitude(),schedule_.lastLongitude(),movementPrecision,
         [](int32_t value,uint32_t bits){return PositionPrecision::truncateCoordinate(value,bits);}));
    // The snapshot interface does not expose a native fix generation. A raw
    // coordinate change gets one immediate smart opportunity; unchanged fixes
    // retry only on the stock 5s worker cadence, never every main-loop tick.
    const bool periodic=!checked_||(uptime>=lastPeriodicCheck_&&uptime-lastPeriodicCheck_>=5000);
    if(periodic){checked_=true;lastPeriodicCheck_=uptime;}
    const bool changed=!observedFix_||p.latitude_i!=observedLatitude_||p.longitude_i!=observedLongitude_;
    observedFix_=true;observedLatitude_=p.latitude_i;observedLongitude_=p.longitude_i;
    if(!periodic&&!changed)return;
    const double moved=PositionDistance::meters(oldLat*1e-7,oldLon*1e-7,lat*1e-7,lon*1e-7);
    const auto kind=schedule_.decide(uint32_t(uptime),policy,moved,periodic);
    if(kind==PositionSchedule::Kind::None||service.announcePositionOnChannel(channelIndex,uint32_t(uptime),random))return;
    service.broadcastAdmitted(history::Kind::Position);
    (void)schedule_.stampSuccess(kind,uint32_t(uptime),p.latitude_i,p.longitude_i);
    if(tracker)modules.noteRoleSleep(g.broadcastSecs);
    if(kind==PositionSchedule::Kind::Periodic&&role==DeviceRolePolicy::Role::LOST_AND_FOUND){
      // The follow-up text uses the same explicitly enabled position channel
      // and the same coarsened coordinates as the position it accompanies.
      char msg[85];const int length=stock::position::lostAndFoundText(lat,lon,msg);
      uint32_t id=0;
      if(length>0&&size_t(length)<sizeof(msg))
        (void)service.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,reinterpret_cast<const uint8_t*>(msg),
          size_t(length),channelIndex,false,uint32_t(uptime),random^0x6c6f7374u,id,true);
    }
  }
};
}
