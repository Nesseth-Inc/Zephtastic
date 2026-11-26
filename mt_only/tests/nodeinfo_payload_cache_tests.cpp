// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdint>
#include "modules/NodeInfoPayloadCache.h"

namespace {
bool sameBytes(const uint8_t* a, const uint8_t* b, uint16_t n) {
  for (uint16_t i = 0; i < n; ++i)
    if (a[i] != b[i]) return false;
  return true;
}
void fillPayload(uint8_t* dst, uint16_t n, uint8_t seed) {
  for (uint16_t i = 0; i < n; ++i)
    dst[i] = static_cast<uint8_t>(seed + i);
}
}  // namespace

int main() {
  using mt::traffic::NodeInfoPayloadCache;

  {  // Group 1: invalid observations refused; cache and caller output unchanged
    NodeInfoPayloadCache c;
    uint8_t payload[234];
    fillPayload(payload, 234, 0x11);
    assert(!c.observe(0u, payload, 233, 1000));        // zero id
    assert(!c.observe(0x1001, nullptr, 233, 1000));    // null bytes
    assert(!c.observe(0x1001, payload, 0, 1000));      // size 0
    assert(!c.observe(0x1001, payload, 234, 1000));    // size 234 > kMaxPayload
    uint8_t out[233]; uint16_t outSize = 0xDEAD;
    assert(!c.copyFresh(0x1001, 1000, out, sizeof(out), outSize));
    assert(outSize == 0xDEAD);                          // caller size untouched
    assert(c.observe(0x1001, payload, 233, 1000));      // valid observe still works
  }

  {  // Group 2: capacity 8; 9th unseen id refused; forget frees exactly one slot
    NodeInfoPayloadCache c;
    uint8_t p[233];
    fillPayload(p, 233, 0x22);
    const uint32_t ids[8] = {0x2001,0x2002,0x2003,0x2004,0x2005,0x2006,0x2007,0x2008};
    for (uint32_t id : ids) assert(c.observe(id, p, 233, 5000));
    for (uint32_t id : ids) {
      uint8_t o[233]; uint16_t s;
      assert(c.copyFresh(id, 5000, o, sizeof(o), s));  // all 8 fresh
    }
    assert(!c.observe(0x2009, p, 233, 5000));          // 9th unseen id refused
    assert(c.forget(0x2002));                          // frees exactly one slot
    for (uint32_t id : ids) {
      if (id == 0x2002) continue;
      uint8_t o[233]; uint16_t s;
      assert(c.copyFresh(id, 5000, o, sizeof(o), s));  // no other id evicted
    }
    assert(c.observe(0x2009, p, 233, 5000));           // now the 9th id fits
  }

  {  // Group 3: full uint64 expiry
    NodeInfoPayloadCache c;
    uint8_t p[233];
    fillPayload(p, 233, 0x33);
    const uint32_t id = 0x3001; uint8_t o[233]; uint16_t s = 0;
    assert(c.observe(id, p, 233, 0));                   // zero timestamp valid
    assert(c.copyFresh(id, 0, o, sizeof(o), s));        // age 0 -> fresh
    assert(c.copyFresh(id, NodeInfoPayloadCache::kMaxAgeMs, o, sizeof(o), s));  // exactly 21600000ms -> fresh
    assert(!c.copyFresh(id, NodeInfoPayloadCache::kMaxAgeMs + 1, o, sizeof(o), s));  // +1ms -> stale
    assert(c.observe(id, p, 233, 1000000));
    assert(!c.copyFresh(id, 500000, o, sizeof(o), s)); // future observation refused
    NodeInfoPayloadCache maxCache;
    assert(maxCache.observe(0x3002, p, 233, UINT64_MAX));  // UINT64_MAX is a valid observation time
    assert(maxCache.copyFresh(0x3002, UINT64_MAX, o, sizeof(o), s));  // exact-now read avoids overflow
  }

  {  // Group 4: re-observe refreshes; reads never refresh; bytes/size exact; bad output refused
    NodeInfoPayloadCache c;
    uint8_t p[233];
    fillPayload(p, 233, 0x44);
    const uint32_t id = 0x4001; uint8_t o[233]; uint16_t s = 0;
    assert(c.observe(id, p, 233, 0) && c.observe(id, p, 233, 1000000));  // re-observe later
    assert(c.copyFresh(id, 1000000ULL + NodeInfoPayloadCache::kMaxAgeMs, o, sizeof(o), s));  // fresh again
    assert(!c.copyFresh(id, 1000000ULL + NodeInfoPayloadCache::kMaxAgeMs + 1, o, sizeof(o), s));  // read did not refresh
    assert(s == 233 && sameBytes(o, p, 233));          // bytes/size exact
    uint8_t small[4] = {0xEE, 0xEE, 0xEE, 0xEE}; uint16_t smallSize = 7;
    assert(!c.copyFresh(id, 1000000ULL + NodeInfoPayloadCache::kMaxAgeMs, small, 4, smallSize));
    assert(smallSize == 7 && small[0] == 0xEE && small[3] == 0xEE);  // caller output untouched
    assert(!c.copyFresh(id, 1000000ULL + NodeInfoPayloadCache::kMaxAgeMs + 1, o, sizeof(o), s));  // failed read did not refresh
    uint16_t nullSize = 9;
    assert(!c.copyFresh(id, 1000000ULL, nullptr, 0, nullSize) && nullSize == 9);  // null refused
  }
  return 0;
}
