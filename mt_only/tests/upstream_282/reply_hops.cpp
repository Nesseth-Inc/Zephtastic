// SPDX-License-Identifier: GPL-3.0-only
// Transport fixture for the pinned stock Unity assertions. Both binaries use
// the same generated packet values and test functions.
#include <cstdint>
#include "unity.h"
#include "HopPolicy.h"

#include <cassert>
#include <meshtastic/mesh.pb.h>
constexpr uint32_t kRequester = 0x22222222;
constexpr uint32_t kLocalNode = 0x11111111;
constexpr unsigned HOP_MAX = 7;
struct { struct { uint8_t hop_limit; } lora; } config;
struct RoutingModule {
    uint8_t getHopLimitForResponse(const meshtastic_MeshPacket &);
} module;
static RoutingModule *testRoutingModule = &module;

#if STOCK_ORACLE
#define USERPREFS_EVENT_MODE 0
struct Default { static uint8_t getConfiguredOrDefaultHopLimit(uint8_t); };
int8_t getHopsAway(const meshtastic_MeshPacket &, int8_t defaultIfUnknown = -1);
uint32_t getFrom(const meshtastic_MeshPacket *p) { return p->from ? p->from : kLocalNode; }
auto *routingModule = testRoutingModule;
#include "stock-implementation.inc"
#else
#include "stock/routing/MeshModule.h"
static void setReplyTo(meshtastic_MeshPacket *reply, const meshtastic_MeshPacket &request)
{
    mt::stock::routing::setReplyTo(*reply, request, kLocalNode, config.lora.hop_limit);
}
uint8_t RoutingModule::getHopLimitForResponse(const meshtastic_MeshPacket &p)
{
    return mt::HopPolicy::response(p.hop_start, p.hop_limit, config.lora.hop_limit,
        p.which_payload_variant == meshtastic_MeshPacket_decoded_tag && p.decoded.has_bitfield);
}
#endif
#include "stock-hop-tests.inc"
