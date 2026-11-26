// SPDX-License-Identifier: GPL-3.0-only
// The only proxy operations are the fixture's favorite flag, a single keyed
// transmit-queue slot and the timing notification. Stock owns each decision.
namespace donor {
struct {
    struct { unsigned role; Mode rebroadcast_mode; } device;
} config;

struct {
    uint32_t favorite = 0;
    bool isFromOrToFavoritedNode(const meshtastic_MeshPacket &p) const {
        return favorite && (p.from == favorite || p.to == favorite);
    }
} database;
auto *nodeDB = &database;
uint32_t getFrom(const meshtastic_MeshPacket *p) { return p->from; }

struct Router {
    inline static uint32_t sender = 0, id = 0;
    inline static bool pending = false;
    static bool cancelSending(uint32_t from, uint32_t packetId) {
        if (!pending || from != sender || id != packetId) return false;
        pending = false;
        return true;
    }
};
struct {
    unsigned calls = 0;
    void clampToLateRebroadcastWindow(uint32_t, uint32_t) { ++calls; }
} timing;
auto *iface = &timing;
struct FloodingRouter {
    unsigned txRelayCanceled = 0;
    bool isRebroadcaster();
    bool roleAllowsCancelingDupe(const meshtastic_MeshPacket *);
    void perhapsCancelDupe(const meshtastic_MeshPacket *);
};
#include "stock-flooding.inc"
} // namespace donor

static void test_zt_forwarding_and_duplicate_queue_against_stock()
{
    constexpr uint32_t from = 0x11223344, id = 201;
    unsigned admissions = 0, duplicates = 0;
    for (unsigned role : {0u, 1u, 2u, 11u, 12u}) for (Mode mode : ALL_MODES) {
        // DeviceConfig normalizes ROUTER/ROUTER_LATE with NONE before this
        // runtime. They cannot occur as a stored role/mode combination.
        if ((role == 2 || role == 11) && mode == meshtastic_Config_DeviceConfig_RebroadcastMode_NONE)
            continue;
        for (bool favorite : {false, true}) {
            auto zt = std::make_unique<Fixture>(81, false);
            auto cfg = zt->service.config;
            cfg.bytes[4] = 1;
            TEST_ASSERT_TRUE(cfg.setDeviceRole(role, mode));
            cfg.seal();
            TEST_ASSERT_TRUE(zt->service.apply(cfg));
            TEST_ASSERT_TRUE(zt->service.start(false, 0));
            TEST_ASSERT_TRUE(zt->service.armEndpoint(true));
            mt::NodeCache::Entry sender{};
            sender.identity(from, 81, 0);
            TEST_ASSERT_TRUE(mt::NodeCache::Entry::text(sender.bytes+sender.Name,40,"Fixture sender",16));
            TEST_ASSERT_TRUE(zt->service.nodes.accept(sender,zt->service.route.identity(),-20,1) >= 0);
            if (favorite)
                TEST_ASSERT_EQUAL_INT(1, zt->service.nodes.edit(from,zt->service.route.identity(),mt::NodeCache::Edit::Favorite));

            const uint8_t data[] = {8,1,18,3,'m','s','g',72,0};
            uint8_t wire[255]{};
            size_t length = 0;
            TEST_ASSERT_TRUE(zt->service.codec.encodeZeroHopData(from,NODENUM_BROADCAST,id,data,sizeof(data),
                                                                  wire,sizeof(wire),length));
            wire[12] = uint8_t(2 | (2 << 5));
            meshtastic_MeshPacket packet{};
            packet.from = from;
            packet.to = NODENUM_BROADCAST;
            packet.id = id;
            packet.hop_start = packet.hop_limit = 2;
            packet.transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;

            donor::FloodingRouter stock;
            donor::config.device.role = role;
            donor::config.device.rebroadcast_mode = mode;
            donor::database.favorite = favorite ? from : 0;
            donor::Router::sender = from;
            donor::Router::id = id;
            donor::Router::pending = stock.isRebroadcaster();
            donor::timing.calls = 0;

            zt->service.rx(wire,length,3,1000,0);
            char context[100];
            snprintf(context,sizeof(context),"role %u, %s, favorite %d",role,modeName(mode),int(favorite));
            TEST_ASSERT_EQUAL_UINT_MESSAGE(donor::Router::pending,zt->service.route.pending(),context);
            ++admissions;
            if (!donor::Router::pending) continue;

            // Same packet ID and ciphertext, received from a different LoRa
            // relay with one fewer hop before the first queued send.
            packet.hop_limit = 1;
            packet.relay_node = 0x77;
            wire[12] = uint8_t(1 | (2 << 5));
            wire[15] = packet.relay_node;
            stock.perhapsCancelDupe(&packet);
            zt->service.rx(wire,length,3,1200,0);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(donor::Router::pending,zt->service.route.pending(),context);
            zt->service.poll(10000,0);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(donor::Router::pending,zt->radio.sends,context);
            ++duplicates;
        }
    }
    TEST_ASSERT_EQUAL_UINT(46, admissions);
    TEST_ASSERT_EQUAL_UINT(32, duplicates);
    printf("PASS %u relay admission and %u duplicate queue/send comparisons against stock FloodingRouter\n",
           admissions, duplicates);
}
