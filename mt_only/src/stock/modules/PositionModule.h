// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic d5f16191, src/modules/PositionModule.cpp and src/mesh/Default.h.
#pragma once
#include <stdint.h>
#include "PeriodicStart.h"

namespace mt::stock::position {
constexpr uint32_t DefaultDedupSeconds = 6 * 60 * 60;
constexpr uint32_t StationaryBroadcastSeconds = DefaultDedupSeconds + 6 * 60;
// The native companion registers NodeInfo and MeshBeacon before Position.
// Both consume a start-delay slot. Tracker constructors skip that stagger.
class PhonePositionPoll {
  uint32_t next_=0;
  bool initialized_=false;
public:
  bool due(uint32_t now,unsigned role){
    if(!initialized_){
      next_=now+(trackerSkipsPositionStagger(role)?0u:periodicStartMs(2));
      initialized_=true;
    }
    if(int32_t(now-next_)<0)return false;
    next_=now+5000;
    return true;
  }
};
// PositionModule::allocReply. Presence is separate from the timestamp so an
// admission at tick zero and an immediate retry cannot bypass the window.
inline bool replyAllowed(uint32_t role,bool haveLast,uint32_t now,uint32_t last)
{
    return role==meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND || !haveLast ||
           uint32_t(now-last)>=3u*60u*1000u;
}

template<class Truncate>
bool positionWithinPrecisionCell(int32_t aLat,int32_t aLon,int32_t bLat,int32_t bLon,
                                 uint32_t precision,Truncate truncateCoordinate)
{
    if (precision == 0 || precision >= 32) return false;
    return truncateCoordinate(aLat,precision) == truncateCoordinate(bLat,precision) &&
           truncateCoordinate(aLon,precision) == truncateCoordinate(bLon,precision);
}
inline uint32_t effectiveBroadcastIntervalMs(uint32_t configured,bool stationary,uint32_t stationaryFloor)
{
    return stationary && stationaryFloor > configured ? stationaryFloor : configured;
}

// allocPositionPacket's field selection after the Zephyr backend supplies a
// current fix. Unknown Zephyr components keep their protobuf presence clear.
template<class Native,class Position>
void selectPositionFields(const Native &n,uint32_t flags,Position &p)
{
    p.has_altitude = (flags & 3u) == 3u && n.measured;
    p.altitude = p.has_altitude ? n.msl() : 0;
    p.has_altitude_hae = (flags & 3u) == 1u && n.measured;
    p.altitude_hae = p.has_altitude_hae ? n.hae() : 0;
    p.has_altitude_geoidal_separation = (flags & 5u) == 5u && n.measured;
    p.altitude_geoidal_separation = p.has_altitude_geoidal_separation ? n.geoid() : 0;
    p.PDOP = p.VDOP = 0;
    p.HDOP = n.measured && (flags & 24u) == 24u ? n.hdop() : 0;
    p.sats_in_view = n.measured && (flags & 32u) ? n.satellites : 0;
    p.timestamp = n.measured && (flags & 128u) && n.hasUtc ? n.utcSeconds : 0;
    p.has_ground_track = n.measured && (flags & 256u) && n.hasGroundTrack && n.groundTrackE5 <= 35999900u;
    p.ground_track = p.has_ground_track ? n.groundTrackE5 : 0;
    p.has_ground_speed = n.measured && (flags & 512u);
    p.ground_speed = p.has_ground_speed ? n.speed() : 0;
}
}
