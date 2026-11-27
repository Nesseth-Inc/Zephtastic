// SPDX-License-Identifier: GPL-3.0-only
#include "HopScalingModule.h" // independently compiled Meshtastic
#include "stock/modules/HopScalingModule.h"
#include <cassert>
#include <cstdio>

StockConfig config{};
struct StockHop : HopScalingModule { using HopScalingModule::runOnce; };

static bool isFromUs(const meshtastic_MeshPacket *p) { return p->from == 42; }
static bool isBroadcast(uint32_t to) { return to == UINT32_MAX || to == UINT32_MAX - 1; }
static uint8_t stockLimit(uint32_t to, meshtastic_PortNum port, unsigned requested)
{
    meshtastic_MeshPacket packet{};
    packet.from = 42;
    packet.to = to;
    packet.hop_limit = requested;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = port;
    auto *p = &packet;
#include "router.inc"
    return packet.hop_limit;
}

int main()
{
    unsigned comparisons = 0;
    for (auto role : {meshtastic_Config_DeviceConfig_Role_CLIENT, meshtastic_Config_DeviceConfig_Role_ROUTER,
                      meshtastic_Config_DeviceConfig_Role_ROUTER_LATE, meshtastic_Config_DeviceConfig_Role_CLIENT_BASE,
                      meshtastic_Config_DeviceConfig_Role_TRACKER, meshtastic_Config_DeviceConfig_Role_TAK_TRACKER,
                      meshtastic_Config_DeviceConfig_Role_SENSOR}) {
        for (unsigned seed : {0u, 0xd8abu}) {
            config.device.role = role;
            StockHop stock;
            mt::stock::HopScalingModule zt;
            hopScalingModule = &stock;
            zt.seed(seed);
            stock.setHashSeed(seed);
            uint32_t now = concurrency::OSThread::startup;
            bool limited = false, recovered = false;
            for (unsigned tick = 0; tick < 12 * 44; ++tick) {
                // First a dense quiet mesh, then bursts on either side of the
                // engage threshold, sustained congestion, deep thinning, and recovery.
                const unsigned hour = tick / 12;
                const unsigned population = hour < 8 ? 85 : hour < 18 ? 400 : hour < 38 ? 3 : 85;
                const float utilization = hour < 4 ? 10.0f : hour < 6 ? (tick % 3 ? 19.5f : 19.49f) :
                                          hour < 8 ? 24.49f : hour < 18 ? 24.5f :
                                          hour < 20 ? (tick % 3 ? 12.49f : 12.5f) : 6.0f;
                for (unsigned i = 0; i < population; ++i) {
                    const uint32_t node = 0x22ad001u + i * 17u;
                    const unsigned distance = hour < 8 ? 0 : (i / 20) % 8;
                    stock.samplePacketForHistogram(node, distance);
                    zt.samplePacketForHistogram(node, distance);
                }
                assert(!zt.due(now - 1) && zt.due(now));
                stock.s_testChannelUtil = utilization;
                const int32_t next = stock.runOnce();
                zt.runOnce(now, utilization, role);
                for (uint32_t to : {UINT32_MAX, 1234u}) {
                    for (auto port : {meshtastic_PortNum_POSITION_APP, meshtastic_PortNum_NODEINFO_APP,
                                      meshtastic_PortNum_TELEMETRY_APP, meshtastic_PortNum_NEIGHBORINFO_APP,
                                      meshtastic_PortNum_MESH_BEACON_APP, meshtastic_PortNum_TEXT_MESSAGE_APP}) {
                        for (unsigned hops = 0; hops <= 7; ++hops) {
                            assert(zt.limit(hops, to, port) == stockLimit(to, port, hops));
                            ++comparisons;
                        }
                    }
                }
                if (hour < 6) assert(stock.getLastRequiredHop() == 7);
                if (stock.getLastRequiredHop() < 7) limited = true;
                if (limited && stock.getLastRequiredHop() == 7 && hour >= 20) recovered = true;
                assert(next > 0);
                now += static_cast<uint32_t>(next);
            }
            assert(limited && recovered);
            hopScalingModule = nullptr;
        }
    }
    printf("%u\n", comparisons);
}
