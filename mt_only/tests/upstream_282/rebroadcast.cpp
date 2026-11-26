// SPDX-License-Identifier: GPL-3.0-only
// Adapt stock's ingress and observation points to a real ZT Service, radio
// recorder and phone serializer. The packet, mode decisions and assertions
// are compiled unchanged from the pinned upstream test and its support file.
#include "../device_config_fixture.h"
#include "unity.h"

using PacketId = uint32_t;
using Mode = meshtastic_Config_DeviceConfig_RebroadcastMode;
constexpr uint32_t ADMIN_NODE = 0x0C0C0C0C, NODENUM_BROADCAST = UINT32_MAX;
#define IS_ONE_OF(value,a,b,c) ((value)==(a)||(value)==(b)||(value)==(c))
static constexpr Mode ALL_MODES[] = {
    meshtastic_Config_DeviceConfig_RebroadcastMode_ALL,
    meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY,
    meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY,
    meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY,
    meshtastic_Config_DeviceConfig_RebroadcastMode_NONE,
};
struct { struct { Mode rebroadcast_mode; } device; } config;

static std::unique_ptr<Fixture> relay;
static unsigned observations;
struct RadioCapture {
    unsigned sendCalls = 0;
    void reset() { sendCalls = 0; }
} capture;
static auto *pipelineRadio = &capture;
struct RouterHistory {
    bool historyContains(const meshtastic_MeshPacket *packet) const {
        return relay->service.route.hasHistory(packet->from, packet->id);
    }
} history;
static auto *pipelineRouter = &history;
struct PhoneQueue {
    size_t cursor = 0;
    meshtastic_MeshPacket *getForPhone() {
        auto &output = relay->usb.out;
        while (cursor < output.size()) {
            auto &message = output[cursor++];
            if (message.which_payload_variant == meshtastic_FromRadio_packet_tag)
                return &message.packet;
        }
        return nullptr;
    }
} phone;
static auto *pipelineService = &phone;
struct { void release(meshtastic_MeshPacket *) {} } packetPool; // USB recorder owns these values.

static void putLE32(uint8_t *wire, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) wire[i] = uint8_t(value >> (8 * i));
}

static void runPipelineIngress(const meshtastic_MeshPacket &packet)
{
    relay = std::make_unique<Fixture>(81, false);
    auto cfg = relay->service.config;
    cfg.bytes[4] = 1;  // provision the bench's test region
    TEST_ASSERT_TRUE(cfg.setDeviceRole(mt::DeviceRolePolicy::Role::CLIENT, config.device.rebroadcast_mode));
    cfg.seal();
    TEST_ASSERT_TRUE(relay->service.apply(cfg));
    TEST_ASSERT_TRUE(relay->service.start(false, 0));
    TEST_ASSERT_TRUE(relay->service.armEndpoint(true));

    meshtastic_ToRadio handshake{};
    handshake.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    handshake.want_config_id = 71;
    relay->input(handshake);
    relay->usb.out.clear();
    phone.cursor = 0;

    uint8_t wire[255]{};
    const size_t size = 16 + packet.encrypted.size;
    TEST_ASSERT_EQUAL(meshtastic_MeshPacket_encrypted_tag, packet.which_payload_variant);
    TEST_ASSERT_TRUE(size <= sizeof(wire));
    putLE32(wire, packet.to);
    putLE32(wire + 4, packet.from);
    putLE32(wire + 8, packet.id);
    wire[12] = uint8_t((packet.hop_start << 5) | packet.hop_limit);
    wire[13] = packet.channel;
    wire[14] = packet.next_hop;
    wire[15] = packet.relay_node;
    memcpy(wire + 16, packet.encrypted.bytes, packet.encrypted.size);
    relay->service.rx(wire, size, 3, 1000, 0);
    const auto queued = relay->service.route.pending();
    relay->c.receive(wire,size,-90,3,1000,mt::Companion::WorkspaceProof{true});
    relay->service.poll(10000,0);
    capture.sendCalls = relay->radio.sends;
    TEST_ASSERT_EQUAL_UINT(queued, capture.sendCalls);
    ++observations;
}

#include "stock-rebroadcast.inc"

void setUp() { observations = 0; }
void tearDown() { relay.reset(); }
int main()
{
    if (mt::DeviceRolePolicy::supportedRebroadcast(meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING))
        return 1;  // This explicit exclusion must be revisited if the firmware gains this mode.
    UNITY_BEGIN();
    RUN_TEST(test_unknown_channel_broadcast_relays_in_core_portnums_only);
    return UNITY_END() || observations != sizeof(ALL_MODES)/sizeof(ALL_MODES[0]);
}
