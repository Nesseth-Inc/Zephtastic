// SPDX-License-Identifier: GPL-3.0-only
#include "HopScalingModule.h" // independently compiled Meshtastic
#include "stock/modules/HopScalingModule.h"
#include "zt-fixture.inc"
#include "TelemetryAnnouncement.h"
#include <cassert>
#include <cstdio>

StockConfig config{};
struct StockHop : HopScalingModule { using HopScalingModule::runOnce; };

struct StockNodeDB { uint32_t getNodeNum() const { return 0x12345678; } };
static StockNodeDB stockNodeDB;
static auto *nodeDB = &stockNodeDB;
#include "node-address.inc"
static uint8_t stockLimit(uint32_t to, meshtastic_PortNum port, unsigned requested)
{
    meshtastic_MeshPacket packet{};
    packet.from = nodeDB->getNodeNum();
    packet.to = to;
    packet.hop_limit = requested;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = port;
    auto *p = &packet;
#include "router.inc"
    return packet.hop_limit;
}

// Compare router policy with the actual encrypted frame handed to the radio,
// using one shared stream of RF samples, 10-second buckets and service ticks.
static unsigned emittedFrames()
{
    Fixture f;
    assert(f.service.route.identity() == nodeDB->getNodeNum());
    StockHop stock;
    config.device.role = static_cast<meshtastic_Config_DeviceConfig_Role>(f.service.config.role());
    hopScalingModule = &stock;
    constexpr uint32_t random = 12345;
    stock.setHashSeed(random);
    uint8_t telemetry[128]{};
    size_t size = 0;
    assert(mt::TelemetryAnnouncement::encode(123, {}, {}, telemetry, sizeof(telemetry), size));
    for (uint32_t peer = 201; peer <= 282; ++peer) {
        uint8_t bytes[255]{};
        size_t length = 0;
        assert(f.service.codec.encodeZeroHopData(peer, f.service.route.identity(), peer,
                                                 telemetry, size, bytes, sizeof(bytes), length));
        bytes[12] = 3u | (3u << 5); // direct peer, zero traversed hops
        f.service.rx(bytes, length, 5, 1000, random, true);
        stock.samplePacketForHistogram(peer, 0);
    }
    uint32_t next = concurrency::OSThread::startup;
    for (uint32_t now = 10000; now <= 3630000; now += 10000) {
        assert(f.clock.set(now));
        f.service.channelMeter().observeRx(3500, true);
        if (now >= next) {
            const auto utilization = f.service.channelMeter().smoothedPercent();
            assert(utilization);
            stock.s_testChannelUtil = *utilization;
            next = now + static_cast<uint32_t>(stock.runOnce());
        }
        f.service.poll(now, random);
    }
    assert(stock.getLastRequiredHop() < 3 && f.radio.frames.empty());

    unsigned comparisons = 0;
    auto verify = [&](uint32_t now, uint32_t to, uint32_t id, meshtastic_PortNum port) {
        assert(f.clock.set(now + 300));
        f.service.poll(now + 300, random);
        assert(f.radio.frames.size() == comparisons + 1);
        const auto &frame = f.radio.frames.back();
        bench::RouteMetadata metadata{};
        assert(f.service.codec.decode(frame.data(), frame.size(), metadata));
        assert(metadata.portnum == unsigned(port) && bench::le32(frame.data()) == to);
        assert(bench::le32(frame.data()+4) == nodeDB->getNodeNum() && bench::le32(frame.data()+8) == id);
        const uint8_t expected = stockLimit(to, port, 3);
        assert((frame[12] & 7u) == expected && (frame[12] >> 5) == expected);
        ++comparisons;
        f.radio.done = true;
        f.service.poll(now + 301, random);
    };

    uint32_t now = 3700000, id = 0;
    assert(f.clock.set(now));
    assert(!f.service.submitModuleAnnouncement(telemetry, size, 3, now, 54321, id));
    verify(now, UINT32_MAX, id, meshtastic_PortNum_TELEMETRY_APP);
    const auto neighbor = meshtastic_PortNum_NEIGHBORINFO_APP;
    uint8_t data[64]{};
    assert(mt::PublicApplication::encode(neighbor, nullptr, 0, 0, 0, data, sizeof(data), size));
    for (uint32_t to : {UINT32_MAX, 0x87654321u}) {
        now += 1500;
        ++id;
        assert(f.clock.set(now));
        assert(!f.service.submitPublicApplication(0, to, id, data, size, 3, false, now));
        verify(now, to, id, neighbor);
    }
    hopScalingModule = nullptr;
    return comparisons;
}

int main()
{
    // Address 1 is explicitly non-LoRa. Its stock broadcast classification
    // must never turn it into an RF frame in the comparison below.
    static_assert(NODENUM_BROADCAST_NO_LORA == 1);
    assert(isBroadcast(1) && !isBroadcast(UINT32_MAX-1));
    const unsigned frames = emittedFrames();
    unsigned comparisons = frames;
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
    printf("{\"comparisons\":%u,\"simulated_radio_frames\":%u}\n", comparisons, frames);
}
