// SPDX-License-Identifier: GPL-3.0-only
// Integrated Qwen fixture with actual local nanopb include/API adaptation.
// Target path: mt_only/tests/traffic_management_window_tests.cpp
// Targets the locked header mt_only/src/modules/TrafficManagementWindow.h;
// Root supplies the generated pb include closure when it compiles this.
#include "../src/modules/TrafficManagementWindow.h"
#include <pb_encode.h>
#include <initializer_list>
#include "meshtastic/config.pb.h"
#include "meshtastic/portnums.pb.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace mt::modules;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                 \
    if (!(cond)) {                                                     \
      ++failures;                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                  \
  } while (0)

// Real encoded Position payload; both coordinate presence flags set.
static size_t encodePosition(uint8_t *buf, size_t cap, int32_t lat, int32_t lon) {
  meshtastic_Position pos{};
  pos.has_latitude_i = true;
  pos.latitude_i = lat;
  pos.has_longitude_i = true;
  pos.longitude_i = lon;
  auto stream=pb_ostream_from_buffer(buf,cap);
  if(!pb_encode(&stream,meshtastic_Position_fields,&pos))return 0;
  return stream.bytes_written;
}

static TrafficManagementSettings makeSettings(uint32_t posSecs, uint32_t rateWindow,
                                              uint32_t rateMax, uint32_t threshold) {
  meshtastic_ModuleConfig_TrafficManagementConfig c{};
  c.position_min_interval_secs = posSecs;
  c.rate_limit_window_secs = rateWindow;
  c.rate_limit_max_packets = rateMax;
  c.unknown_packet_threshold = threshold;
  TrafficManagementSettings s;
  s.set(c);
  return s;
}

static const uint8_t kText[] = {0x11, 0x22, 0x33};

int main() {
  uint8_t bufA[64], bufB[64];
  const size_t lenA = encodePosition(bufA, sizeof(bufA), 47631000, -12234000);
  const size_t lenB = encodePosition(bufB, sizeof(bufB), -33870000, 15121000);
  CHECK(lenA&&lenB);

  // Group 1: configuration and admission.
  {
    TrafficManagementWindow unconfigured;
    CHECK(!unconfigured.decoded(1, 2, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, 0));
    CHECK(!unconfigured.unknown(1, 0));
    TrafficManagementWindow disabled;
    disabled.configure(makeSettings(0, 0, 0, 0));
    CHECK(!disabled.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 0));
    CHECK(!disabled.unknown(1, 0));
    TrafficManagementWindow win;
    win.configure(makeSettings(0, 300, 1, 0));
    // A false return only means the gate did not drop the packet; it is not
    // evidence of forwarding or transmission.
    CHECK(!win.decoded(1, 2, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, 0));
    CHECK(win.decoded(1, 2, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, 1000));
    // ADMIN and ROUTING are excluded from rate counting even for a rate-denied source.
    CHECK(!win.decoded(1, 2, 1000, meshtastic_PortNum_ROUTING_APP, kText, sizeof(kText), false, 5, 0, 2000));
    CHECK(!win.decoded(1, 2, 1000, meshtastic_PortNum_ADMIN_APP, kText, sizeof(kText), false, 5, 0, 3000));
    CHECK(!win.decoded(1000, 2, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, 4000));
    CHECK(!win.decoded(101, 1000, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, 5000));
  }

  // Group 2: position gate.
  {
    TrafficManagementWindow win;
    win.configure(makeSettings(3600, 0, 0, 0));  // 10 six-minute ticks, uncapped role
    const uint32_t role = 0;
    CHECK(!win.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 0));
    CHECK(win.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 360000));
    CHECK(win.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 3240000));
    CHECK(!win.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 3600000));  // 10 >= 10 ticks: allowed, so the denied duplicates did not slide the stamp
    CHECK(!win.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufB, lenB, true, 5, role, 3960000));  // clearly different cell is allowed
    CHECK(win.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufB, lenB, true, 5, role, 4320000));
    for (const uint32_t capped : {uint32_t(meshtastic_Config_DeviceConfig_Role_TRACKER),
                                  uint32_t(meshtastic_Config_DeviceConfig_Role_TAK_TRACKER)}) {
      TrafficManagementWindow cap;
      cap.configure(makeSettings(UINT32_MAX, 0, 0, 0));
      CHECK(!cap.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, capped, 0));
      CHECK(cap.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, capped, 3240000));  // 9 < 10 capped ticks
      CHECK(!cap.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, capped, 3600000));  // 10 >= 10
    }
    {
      TrafficManagementWindow lost;
      lost.configure(makeSettings(UINT32_MAX, 0, 0, 0));
      const uint32_t role = uint32_t(meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND);
      CHECK(!lost.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 0));
      CHECK(lost.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 360000));  // 1 < 2 capped ticks
      CHECK(!lost.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, role, 720000));  // 2 >= 2
    }
    {
      TrafficManagementWindow off;
      off.configure(makeSettings(0, 0, 0, 0));
      CHECK(!off.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 0));
      CHECK(!off.decoded(1, 2, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 360000));
    }
  }

  // Group 3: long idle and full clock.
  {
    // Rate window 4800s clamps to 15 five-minute ticks; unknown threshold 2;
    // position interval UINT32_MAX clamps to the 255-tick compact maximum.
    TrafficManagementWindow win;
    win.configure(makeSettings(UINT32_MAX, 4800, 1, 2));
    auto rate = [&](uint64_t now) {
      return win.decoded(1, 77, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, now);
    };
    CHECK(!rate(0));
    CHECK(rate(300000));
    CHECK(!rate(5400000));  // 85 min after the denial; clamped 15-tick window has elapsed
    CHECK(!win.unknown(1, 5460000));
    CHECK(!win.unknown(1, 5520000));
    CHECK(win.unknown(1, 5580000));
    CHECK(!win.unknown(1, 6540000));  // 16 min after the denial; low nibble aliases tick 93 but the uint64 anchor resets
    CHECK(!win.decoded(1, 77, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 9900000));
    CHECK(win.decoded(1, 77, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 96300000));  // 24 h later: 240 < 255 compact ticks
    CHECK(!win.decoded(1, 77, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 103500000));  // 26 h later: 260 >= 255
    CHECK(!rate(4300000000ULL));  // ~49.8 days > UINT32_MAX; ticks formed in uint64_t without overflow
    CHECK(!rate(1000));  // backward timestamp clears every source slot
    CHECK(rate(2000));  // counter restarted at zero
    CHECK(!win.unknown(1, 2500));
    CHECK(!win.decoded(1, 77, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 4500000));  // position stamp restarted at tick 12
    CHECK(win.decoded(1, 77, 1000, meshtastic_PortNum_POSITION_APP, bufA, lenA, true, 5, 0, 4860000));
  }

  // Group 4: 16-slot LRU capacity.
  {
    TrafficManagementWindow win;
    win.configure(makeSettings(0, 300, 1, 0));
    auto pkt = [&](uint32_t src, uint64_t now) {
      return win.decoded(src, 2000, 1000, meshtastic_PortNum_TEXT_MESSAGE_APP, kText, sizeof(kText), false, 5, 0, now);
    };
    CHECK(!pkt(1, 1000));
    CHECK(pkt(1, 1100));  // source 1 rate-denied; its stamp stays the oldest, so it is the LRU
    for (uint32_t src = 2; src <= 16; ++src) CHECK(!pkt(src, uint64_t(1000 * src)));
    CHECK(!pkt(17, 18000));  // evicts the least-recent slot (source 1)
    CHECK(!pkt(1, 19000));  // fresh slot: fresh allowance after eviction
    CHECK(pkt(1, 20000));
    CHECK(pkt(17, 21000));  // source 17's slot survived; its denial persists
  }

  // Group 5: deleting an earlier slot must preserve every later source gate.
  {
    TrafficManagementWindow rate;
    rate.configure(makeSettings(0,300,1,0));
    auto packet=[&](uint32_t src,uint64_t now){return rate.decoded(src,77,1000,meshtastic_PortNum_TEXT_MESSAGE_APP,kText,sizeof(kText),false,5,0,now);};
    CHECK(!packet(1,0));CHECK(!packet(2,1));CHECK(packet(2,2));CHECK(rate.forget(1));CHECK(packet(2,3));
    CHECK(rate.forget(2));CHECK(!packet(2,4));
    TrafficManagementWindow unknown;unknown.configure(makeSettings(0,0,0,1));
    CHECK(!unknown.unknown(1,0));CHECK(!unknown.unknown(2,1));CHECK(unknown.unknown(2,2));
    CHECK(unknown.forget(1));CHECK(unknown.unknown(2,3));CHECK(unknown.forget(2));CHECK(!unknown.unknown(2,4));
    TrafficManagementWindow position;position.configure(makeSettings(18000,0,0,0));
    auto pos=[&](uint32_t src,uint64_t now){return position.decoded(src,77,1000,meshtastic_PortNum_POSITION_APP,bufA,lenA,true,5,0,now);};
    CHECK(!pos(1,0));CHECK(!pos(2,1));CHECK(pos(2,2));CHECK(position.forget(1));CHECK(pos(2,3));
    CHECK(position.forget(2));CHECK(!pos(2,4));CHECK(!position.forget(0));position.observeRole(0,5);
  }

  std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
  return failures ? 1 : 0;
}
