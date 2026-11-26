// SPDX-License-Identifier: GPL-3.0-only
#include "stock/modules/NodeInfoModule.h"
#include "stock/modules/PositionModule.h"
#include "gnss/GnssMeasuredProjection.h"
#include <meshtastic/config.pb.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace oracle {
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
uint32_t now=0,scaledFloor=0;
struct {struct {uint32_t role=0,node_info_broadcast_secs=0;} device;} config;
constexpr uint32_t default_node_info_broadcast_secs=10800;
struct Default {
 static uint32_t getConfiguredOrDefaultMsScaled(uint32_t,uint32_t,uint32_t){return scaledFloor;}
 static uint32_t getConfiguredOrDefaultMs(uint32_t value,uint32_t fallback){return (value?value:fallback)*1000u;}
};
struct {bool allow=true;bool isTxAllowedChannelUtil(bool polite){return allow&&!polite;}} air;
auto *airTime=&air;
struct {unsigned getNumOnline(){return 1;}} status;
auto *nodeStatus=&status;
struct {uint32_t last=0;
 uint32_t getLastSentToMeshMillis(unsigned){return last;} void setLastSentToMesh(unsigned){last=now;}
} history;
auto *transmitHistory=&history;
struct Throttle {static bool isWithinTimespanMs(uint32_t then,uint32_t ms){return uint32_t(now-then)<ms;}};
bool isFromUs(const meshtastic_MeshPacket *p){return p->from==42;}
meshtastic_User owner{};
struct {struct Id{const char *c_str()const{return "!0000002a";}};Id getNodeId(){return {};}} db;
auto *nodeDB=&db;
struct NodeInfoModule {
 meshtastic_MeshPacket *currentRequest=nullptr,out{};
 bool ignoreRequest=false,suppressReplyForCurrentRequest=false,shorterTimeout=false,deferHistoryStamp=true;
 meshtastic_MeshPacket *allocDataProtobuf(const meshtastic_User &){return &out;}
 meshtastic_MeshPacket *allocReply();
};
struct Time {static uint32_t skipZero(uint32_t v){return v?v:1u;} static uint32_t getMillis(){return now;}};
struct PositionModule {
 uint32_t lastSentReply=0,precision=0;
 bool ignoreRequest=false;
 meshtastic_MeshPacket packet{};
 meshtastic_MeshPacket *allocPositionPacket(uint32_t){return &packet;}
 meshtastic_MeshPacket *allocReply();
 static bool positionWithinPrecisionCell(int32_t,int32_t,int32_t,int32_t,uint32_t);
 static uint32_t effectiveBroadcastIntervalMs(uint32_t,bool,uint32_t);
};
#include "oracle.inc"
}

int main(){
  static_assert(mt::stock::position::DefaultDedupSeconds == default_traffic_mgmt_position_min_interval_secs);
  static_assert(mt::stock::position::StationaryBroadcastSeconds == default_position_stationary_broadcast_secs);
  unsigned nodeCases=0,positionCases=0;
  for(uint32_t then:{0u,1u,UINT32_MAX-200000u}) for(uint32_t elapsed:{0u,59999u,60000u,599999u,600000u,1799999u,1800000u,3600000u})
  for(bool channel:{false,true}) for(bool shorter:{false,true}) for(bool licensed:{false,true})
  for(uint32_t scaled:{1800000u,3600000u}){
    oracle::NodeInfoModule donor;donor.shorterTimeout=shorter;
    oracle::owner.is_licensed=licensed;oracle::air.allow=channel;
    oracle::history.last=then;oracle::now=then+elapsed;oracle::scaledFloor=scaled;
    oracle::config.device.node_info_broadcast_secs=600;
    const bool expected=donor.allocReply()!=nullptr;
    assert(expected==mt::stock::nodeinfo::allocReply(channel,then!=0,oracle::now,then,scaled,shorter,licensed,600000));
    ++nodeCases;
  }
  for(uint32_t role=0;role<13;++role) for(bool want:{false,true}){
    oracle::config.device.role=role;
    assert(mt::stock::nodeinfo::sendOurNodeInfo(want,role)==oracle::wantRepliesForRole(want));++nodeCases;
  }
  for(uint32_t role=0;role<=12;++role) for(uint32_t last:{0u,1u,0xfffffff0u})
  for(uint32_t elapsed:{0u,1u,179999u,180000u,180001u}) {
    oracle::config.device.role=role;oracle::now=last+elapsed;
    oracle::PositionModule donor;donor.lastSentReply=last;
    assert((donor.allocReply()!=nullptr)==mt::stock::position::replyAllowed(role,last!=0,oracle::now,last));
    ++positionCases;
  }
  zt::gnss::PromotionEvent f{};
  f.measured=true;f.hasUtc=true;f.utcSeconds=1700000000;f.satellites=7;f.hdopMilli=1234;
  f.altitudeMm=-3200;f.geoidMm=8300;f.hasGroundTrack=true;f.groundTrackE5=0;f.speedMmPerSecond=2500;
  meshtastic_Position source{};
  source.altitude=-3;source.altitude_hae=5;source.altitude_geoidal_separation=8;source.sats_in_view=7;
  source.timestamp=f.utcSeconds;source.ground_speed=9;source.ground_track=0;source.HDOP=123;
  for(uint32_t flags=0;flags<1024;++flags){
    meshtastic_Position actual{};zt::gnss::projectMeasurements(f,flags,actual);
    auto expected=oracle::emitFields(source,flags);
    assert(actual.has_altitude==expected.has_altitude && actual.altitude==expected.altitude &&
      actual.has_altitude_hae==expected.has_altitude_hae && actual.altitude_hae==expected.altitude_hae &&
      actual.has_altitude_geoidal_separation==expected.has_altitude_geoidal_separation &&
      actual.altitude_geoidal_separation==expected.altitude_geoidal_separation &&
      actual.sats_in_view==expected.sats_in_view && actual.timestamp==expected.timestamp &&
      actual.has_ground_speed==expected.has_ground_speed && actual.ground_speed==expected.ground_speed &&
      actual.has_ground_track==expected.has_ground_track && actual.ground_track==expected.ground_track &&
      actual.PDOP==expected.PDOP && actual.VDOP==expected.VDOP && actual.HDOP==expected.HDOP);
    ++positionCases;
  }
  f.measured=false;meshtastic_Position absent=source;zt::gnss::projectMeasurements(f,1023,absent);
  assert(!absent.has_ground_track&&!absent.has_ground_speed&&!absent.has_altitude&&!absent.has_altitude_hae&&absent.HDOP==0);
  for(unsigned bits=0;bits<=33;++bits) for(int32_t lat:{-1800000000,-1,0,1,1799999999})
  for(int32_t delta:{-7,0,7}){
    assert(mt::stock::position::positionWithinPrecisionCell(lat,lat,lat+delta,lat-delta,bits,oracle::truncateCoordinate)==
           oracle::PositionModule::positionWithinPrecisionCell(lat,lat,lat+delta,lat-delta,bits));++positionCases;
  }
  for(bool stationary:{false,true})for(uint32_t interval:{1000u,21600000u,43200000u})
    assert(mt::stock::position::effectiveBroadcastIntervalMs(interval,stationary,mt::stock::position::StationaryBroadcastSeconds*1000)==
           oracle::PositionModule::effectiveBroadcastIntervalMs(interval,stationary,default_position_stationary_broadcast_secs*1000));
  printf("PASS %u NodeInfo admission/role and %u Position reply/precision/measurement cases\n",nodeCases,positionCases);
}
