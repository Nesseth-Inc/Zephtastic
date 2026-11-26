// SPDX-License-Identifier: GPL-3.0-only
#include "gnss/GnssClockProjection.h"
#include "gnss/GnssMeasuredProjection.h"
#include "WallClock.h"
#include <cassert>
#include <cstdio>
using namespace zt::gnss;
int main(){
  PromotionEvent fix{41000000000LL,-93000000000LL,10000,0,5,true};
  fix.measured=true;fix.hasUtc=true;fix.utcSeconds=1700000000;
  assert(projectUtc(fix,10000,120000)==1700000000);
  assert(projectUtc(fix,11999,120000)==1700000001);
  assert(projectUtc(fix,130000,120000)==1700000120);
  assert(!projectUtc(fix,130001,120000)&&!projectUtc(fix,9999,120000));
  auto invalid=fix;invalid.hasUtc=false;assert(!projectUtc(invalid,10000,120000));
  invalid=fix;invalid.measured=false;assert(!projectUtc(invalid,10000,120000));
  invalid=fix;invalid.utcSeconds=0;assert(!projectUtc(invalid,10000,120000));
  invalid=fix;invalid.valid=false;assert(!projectUtc(invalid,10000,120000));
  invalid=fix;invalid.satellites=3;assert(!projectUtc(invalid,10000,120000));
  invalid=fix;invalid.latitudeNdeg=0;invalid.longitudeNdeg=0;assert(!projectUtc(invalid,10000,120000));
  invalid=fix;invalid.utcSeconds=UINT32_MAX;assert(projectUtc(invalid,10999,120000)==UINT32_MAX);
  assert(!projectUtc(invalid,11000,120000));
  // No selected RF timestamp, but local time still uses the admitted fix.
  meshtastic_Position position{};assert(projectPosition(fix,10000,120000,position));
  projectMeasurements(fix,0,position);assert(!position.timestamp&&projectUtc(fix,10000,120000));
  mt::WallClock clock;assert(clock.set(projectUtc(fix,10000,120000),10000)==0);
  assert(clock.at(11999)==1700000001);
  // A recent companion/phone anchor wins over a discrepant GNSS observation.
  mt::WallClock phone;assert(phone.set(1700000500,10000)==0);
  assert(phone.set(projectUtc(fix,11000,120000),11000)==-EAGAIN);
  assert(phone.at(11000)==1700000501);
  assert(phone.set(phone.at(11000),11000)==0);
  auto fresh=fix;fresh.capturedUptimeMs=1810000;fresh.utcSeconds=1700001800;
  assert(phone.set(projectUtc(fresh,1810000,120000),1810000)==0);
  assert(phone.at(1811000)==1700001801);
  puts("PASS GNSS clock: fresh promoted UTC independent of RF flags, age interpolation, absence/geography/overflow refusal, phone correction guard and exact boundary");
}
