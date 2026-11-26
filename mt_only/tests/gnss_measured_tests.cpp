// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/GnssPromotionProjection.h"
#include "gnss/GnssMeasuredProjection.h"
#include <cassert>
#include <cstdio>
using namespace zt::gnss;
int main() {
  PromotionEvent e{12345678900LL,-23456789000LL,1000,545400,8,true};
  e.measured=true;e.hasUtc=true;e.utcSeconds=1709164800;e.geoidMm=46900;e.speedMmPerSecond=10000;
  e.hdopMilli=999;static_assert(sizeof(PromotionEvent)==56);
  for(uint32_t flags=0;flags<1024;++flags) {
    meshtastic_Position p{};assert(projectPosition(e,1000,0,p));projectMeasurements(e,flags,p);
    assert(p.has_latitude_i&&p.has_longitude_i&&p.latitude_i==123456789&&p.longitude_i==-234567890);
    assert(p.has_altitude==bool((flags&1u)&&(flags&2u)));
    assert(p.has_altitude_hae==bool((flags&1u)&&!(flags&2u)));
    assert(p.has_altitude_geoidal_separation==bool((flags&1u)&&(flags&4u)));
    if(p.has_altitude)assert(p.altitude==545);
    if(p.has_altitude_hae)assert(p.altitude_hae==592);
    if(p.has_altitude_geoidal_separation)assert(p.altitude_geoidal_separation==46);
    assert(p.timestamp==((flags&128u)?1709164800u:0));
    assert(p.sats_in_view==((flags&32u)?8u:0));
    assert(p.has_ground_speed==bool(flags&512u)&&p.ground_speed==((flags&512u)?36u:0));
    assert(p.HDOP==((flags&24u)==24u?99u:0u));
    assert(!p.time&&!p.has_ground_track&&!p.PDOP&&!p.VDOP&&!p.seq_number);
  }
  puts("PASS all1024 stock flag selections preserve geography and truthful optional fields");
  meshtastic_Position p{};e.measured=false;projectMeasurements(e,679,p);
  assert(!p.has_altitude&&!p.has_altitude_hae&&!p.has_altitude_geoidal_separation&&!p.has_ground_speed&&!p.timestamp&&!p.sats_in_view);
  e.measured=true;e.hasUtc=false;e.speedMmPerSecond=0;projectMeasurements(e,679,p);
  assert(!p.timestamp&&p.has_ground_speed&&p.ground_speed==0);
  puts("PASS absent measurements and UTC clear previous values; measured zero speed remains present");
  e.measured=true;e.hasGroundTrack=true;e.groundTrackE5=0;projectMeasurements(e,256,p);
  assert(p.has_ground_track&&p.ground_track==0); // measured north remains present
  e.groundTrackE5=35999900;projectMeasurements(e,256,p);assert(p.has_ground_track&&p.ground_track==35999900);
  e.groundTrackE5=36000000;projectMeasurements(e,256,p);assert(!p.has_ground_track&&!p.ground_track);
  e.groundTrackE5=18000000;projectMeasurements(e,0,p);assert(!p.has_ground_track&&!p.ground_track);
  e.hasGroundTrack=false;projectMeasurements(e,256,p);assert(!p.has_ground_track&&!p.ground_track);
  puts("PASS heading presence preserves measured north and rejects absent/out-of-range ground track");
  e.altitudeMm=INT32_MAX;e.geoidMm=INT32_MAX;projectMeasurements(e,1,p);assert(p.altitude_hae==4294967);
  e.altitudeMm=INT32_MIN;e.geoidMm=INT32_MIN;projectMeasurements(e,1,p);assert(p.altitude_hae==-4294967);
  e.altitudeMm=-1500;e.geoidMm=-999;projectMeasurements(e,7,p);assert(p.altitude==-1&&p.altitude_geoidal_separation==0);
  assert(millimetersPerSecondToKmh(UINT32_MAX)==15461882u);
  e.hdopMilli=UINT16_MAX;projectMeasurements(e,24,p);assert(p.HDOP==6553&&!p.PDOP&&!p.VDOP);
  e.hdopMilli=0;projectMeasurements(e,24,p);assert(!p.HDOP);
  puts("PASS derived HAE addition and speed scaling use wide arithmetic; signed truncation is defined");
}
