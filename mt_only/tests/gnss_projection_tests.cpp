// SPDX-License-Identifier: GPL-3.0-only
// Synthetic geography/freshness vectors, no receiver observation.
#include "gnss/GnssPromotionProjection.h"
#include <assert.h>
#include <stdint.h>
using namespace zt::gnss;
static bool empty(const meshtastic_Position &p) {
  return !p.has_latitude_i&&!p.has_longitude_i&&!p.has_altitude&&p.latitude_i==0&&p.longitude_i==0&&p.altitude==0&&p.time==0&&p.location_source==0;
}
static PromotionEvent sample() {return {12345678900LL,-23456789000LL,1000,12345,5,true};}
int main() {
  auto e=sample();meshtastic_Position p{};
  assert(projectPosition(e,2000,1000,p));
  assert(p.has_latitude_i&&p.has_longitude_i&&p.latitude_i==123456789&&p.longitude_i==-234567890);
  assert(!p.has_altitude&&p.time==0&&p.location_source==meshtastic_Position_LocSource_LOC_INTERNAL);
  assert(!projectPosition(e,2001,1000,p)&&empty(p));
  assert(!projectPosition(e,999,1000,p)&&empty(p));
  assert(!projectPosition(e,-1,1000,p)&&empty(p));
  assert(!projectPosition(e,1000,-1,p)&&empty(p));
  const int64_t latitudes[]={-90000000000LL,90000000000LL};
  for(int64_t lat:latitudes) {
    e=sample();e.latitudeNdeg=lat;assert(projectPosition(e,1000,0,p));assert(p.latitude_i==lat/100);
    e.latitudeNdeg=lat+(lat<0?-1:1);assert(!projectPosition(e,1000,0,p)&&empty(p));
  }
  const int64_t longitudes[]={-180000000000LL,180000000000LL};
  for(int64_t lon:longitudes) {
    e=sample();e.longitudeNdeg=lon;assert(projectPosition(e,1000,0,p));assert(p.longitude_i==lon/100);
    e.longitudeNdeg=lon+(lon<0?-1:1);assert(!projectPosition(e,1000,0,p)&&empty(p));
  }
  for(int bad=0;bad<4;++bad) {
    e=sample();if(bad==0)e.valid=false;if(bad==1)e.satellites=3;if(bad==2)e.capturedUptimeMs=-1;if(bad==3)e.latitudeNdeg=e.longitudeNdeg=0;
    p.has_latitude_i=true;p.latitude_i=42;p.has_altitude=true;p.altitude=777;
    assert(!projectPosition(e,2000,1000,p)&&empty(p));
  }
  e=sample();e.latitudeNdeg=-199;e.longitudeNdeg=199;assert(projectPosition(e,1000,0,p));assert(p.latitude_i==-1&&p.longitude_i==1);
  e=sample();e.capturedUptimeMs=0;assert(projectPosition(e,INT64_MAX,INT64_MAX,p));
  PendingPromotion pending{};PromotionEvent out{};e=sample();
  assert(publishPromotion(pending,e));auto other=e;other.latitudeNdeg=42;
  assert(!publishPromotion(pending,other));assert(takePromotion(pending,out));
  assert(out.latitudeNdeg==e.latitudeNdeg&&out.longitudeNdeg==e.longitudeNdeg&&out.capturedUptimeMs==e.capturedUptimeMs&&out.altitudeMm==e.altitudeMm&&out.satellites==e.satellites&&out.valid==e.valid);
  assert(!pending.pending&&!pending.event.valid&&pending.event.latitudeNdeg==0&&pending.event.altitudeMm==0);
  assert(!takePromotion(pending,out)&&!out.valid&&out.latitudeNdeg==0&&out.satellites==0);
}
