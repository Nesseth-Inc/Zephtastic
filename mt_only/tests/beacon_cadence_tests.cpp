// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdint>
#include "modules/BeaconCadence.h"
int main() {
  using mt::beacon::BeaconCadence;
  constexpr uint32_t h = 3600u;
  constexpr uint64_t ms = 3600000ULL;
  assert(BeaconCadence::intervalMs(0u) == ms);
  assert(BeaconCadence::intervalMs(h - 1u) == ms);
  assert(BeaconCadence::intervalMs(UINT32_MAX) == uint64_t(UINT32_MAX) * 1000ULL);
  BeaconCadence noStored;
  assert(noStored.due(0u, 0u, false, 0u, h));
  assert(noStored.due(0u, 0u, false, 0u, UINT32_MAX));
  BeaconCadence stored;
  assert(!stored.due(ms - 1u, 0u, true, 100u, h));
  assert(!stored.due(ms, 3699u, true, 100u, h));
  assert(stored.due(ms, 3700u, true, 100u, h));
  assert(!stored.due(ms - 1u, 99u, true, 100u, h));
  assert(stored.due(ms, 99u, true, 100u, h));
  assert(!stored.due(ms - 1u, 100u, true, 0u, h));
  assert(stored.due(ms, 100u, true, 0u, h));
  BeaconCadence both;
  assert(both.attempted(0u));
  assert(!both.due(ms, 100u, true, 100u, h));
  assert(both.due(ms * 2u, 3700u, true, 100u, h));
  assert(!both.due(UINT64_MAX, 0u, false, 0u, h));
  assert(!both.attempted(UINT64_MAX));
  assert(both.attempted(ms * 2u));
  assert(!both.attempted(ms));
  assert(!both.due(ms, 0u, false, 0u, h));
  BeaconCadence wide;const uint64_t start=uint64_t(UINT32_MAX)-100;
  assert(wide.attempted(start));
  assert(!wide.due(start+ms-1,0,false,0,h));
  assert(wide.due(start+ms,0,false,0,h));
  return 0;
}
