// SPDX-License-Identifier: GPL-3.0-only
// stock_position_dedup_tests.cpp
// M112 fixture proposal for mt::traffic::StockPositionDedup (locked API).
// Integrated fixture; execution evidence is recorded by the actual test runner.
#include "modules/StockPositionDedup.h"
#include <cassert>
#include <cstdint>

using mt::traffic::StockPositionDedup;

// Role codes from generated meshtastic/config.pb.h, used as integer codes.
static constexpr uint32_t kNormal    = 0;
static constexpr uint32_t kTracker   = 5;   // TRACKER
static constexpr uint32_t kLostFound = 9;   // LOST_AND_FOUND
static constexpr uint32_t kTakTracker= 10;  // TAK_TRACKER
static constexpr int32_t  kLat  =  418000000;  // ~41.8 deg (1e-7 deg units)
static constexpr int32_t  kLon  = -876000000;  // ~-87.6 deg
static constexpr uint32_t kPrec = 19;      // default precision
static constexpr uint32_t kBase = 18000;   // base interval seconds (50 ticks @360s)

int main() {
  // (1) fingerprint() oracle from pinned truncateCoordinate() and
  // computePositionFingerprint(): fallback p0/>32 to default19; center-truncate,
  // take low min(p,4) bits from both shifted coordinates, pack lat<<4|lon, 0->FF.
  assert(StockPositionDedup::fingerprint(kLat, kLon, 0)  == 0x1A); // p0 -> default precision 19
  assert(StockPositionDedup::fingerprint(kLat, kLon, 1)  == 0x01); // source formula
  assert(StockPositionDedup::fingerprint(kLat, kLon, 4)  == 0x1C);
  assert(StockPositionDedup::fingerprint(kLat, kLon, 19) == 0x1A);
  assert(StockPositionDedup::fingerprint(kLat, kLon, 32) == 0xFF);
  assert(StockPositionDedup::fingerprint(-kLat, kLon, 19) == 0xEA); // negative coordinate
  assert(StockPositionDedup::fingerprint(123, 123, 32) == 0xBB);    // actual precision-32 oracle
  assert(StockPositionDedup::fingerprint(0, 0, 19) == 0xFF);        // computed-zero -> 0xFF

  StockPositionDedup d;

  // (2) raw interval zero / missing coords / ordinary duplicate
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, 0, kNormal, 0)); // interval 0 disables drop
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, 0, kNormal, 0)); // repeatedly allows
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, 0, kNormal, 5));
  d.reset();
  assert(!d.shouldDrop(false, true, kLat, kLon, kPrec, kBase, kNormal, 0)); // no lat: allow, no seed
  assert(!d.shouldDrop(true, false, kLat, kLon, kPrec, kBase, kNormal, 0)); // no lon: allow, no seed
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 0));  // seed (allow)
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 1));  // dup, elapsed 1<50
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 9));  // elapsed 9<50
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 50)); // boundary 50 -> allow

  // (3) role caps, base 18000s: TRACKER/TAK 10 ticks, LOST_AND_FOUND 2, normal 50
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kTracker, 0));
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kTracker, 9));  // 9<10 drop
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kTracker, 10)); // boundary 10
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kTakTracker, 0));
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kTakTracker, 9));
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kTakTracker, 10));
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kLostFound, 0));
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kLostFound, 1)); // 1<2 drop
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kLostFound, 2));  // boundary 2
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 0));
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 49));    // 49<50 drop
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 50));    // boundary 50

  // (4) uint8 wrap + non-sliding + reset clears one fixed slot
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 250)); // seed at 250
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 255)); // (255-250)=5<50 drop
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 43));  // wrap (43-250)&0xFF=49<50
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 44));  // wrap boundary 50 -> allow
  assert( d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 45));  // re-anchored at 44, 1<50
  d.reset();
  assert(!d.shouldDrop(true, true, kLat, kLon, kPrec, kBase, kNormal, 45));  // reset cleared fp/time -> seed
  return 0;
}
