// SPDX-License-Identifier: GPL-3.0-only
#include "zt-fixture.inc"
#include <cstdio>

// Pinned production methods own all selection, serialization and scheduling.
// Inject a single US LongFast radio, its clock, packet pool and in-memory history.
namespace donor {
using Region = meshtastic_Config_LoRaConfig_RegionCode;
using Preset = meshtastic_Config_LoRaConfig_ModemPreset;
using BeaconConfig = meshtastic_ModuleConfig_MeshBeaconConfig;
using ChannelIndex = uint8_t;
constexpr uint32_t NODENUM_BROADCAST = UINT32_MAX;
struct { BeaconConfig mesh_beacon{}; } moduleConfig;
struct { meshtastic_Config_LoRaConfig lora{}; meshtastic_Config_DeviceConfig device{}; } config;
struct RegionInfo {
    Region code = meshtastic_Config_LoRaConfig_RegionCode_US;
    Preset getDefaultPreset() const { return meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST; }
};
static const RegionInfo *getRegion(Region) { static const RegionInfo us; return &us; }
struct RadioInterface {
    static uint32_t frequencySlotCount(const meshtastic_Config_LoRaConfig &) { return 104; }
    static uint32_t resolveFrequencySlot(const meshtastic_Config_LoRaConfig &r, const char *) {
        return r.channel_num && r.channel_num <= frequencySlotCount(r) ? r.channel_num : 20;
    }
};
struct DisplayFormatters {
    static const char *getModemPresetDisplayName(Preset, bool, bool) { return "LongFast"; }
};
struct {
    meshtastic_Channel primary{};
    ChannelIndex getPrimaryIndex() const { return 0; }
    const char *getName(ChannelIndex) const { return "LongFast"; }
    unsigned getNumChannels() const { return 1; }
    const meshtastic_Channel &getByIndex(ChannelIndex) const { return primary; }
    int16_t hashFor(ChannelIndex, const char *) const { return 0; }
} channels;
struct Clock { static uint32_t getMillis() { return ms; } static uint32_t ms; };
using Time = Clock;
uint32_t Clock::ms = 0;
struct Default {
    static uint32_t getConfiguredOrDefault(uint32_t, uint32_t);
    static uint32_t getConfiguredOrMinimumValue(uint32_t, uint32_t);
};
struct Throttle { static bool isWithinTimespanMs(uint32_t, uint32_t); };
struct History {
    uint32_t last = 0;
    uint32_t getLastSentToMeshMillis(meshtastic_PortNum) const { return last; }
    void setLastSentToMesh(meshtastic_PortNum) { last = Time::ms; }
} history;
auto *transmitHistory = &history;
struct AirTime { bool allowed = true; bool isTxAllowedAirUtil() const { return allowed; } } air;
auto *airTime = &air;
struct NodeDB { uint32_t getNodeNum() const { return 0x12345678; } } nodes;
auto *nodeDB = &nodes;
struct { void release(meshtastic_MeshPacket *) {} } packetPool;
static void stampRxTime(meshtastic_MeshPacket *) {}
static size_t pb_encode_to_bytes(uint8_t *out, size_t capacity, const pb_msgdesc_t *type, const void *src) {
    auto stream = pb_ostream_from_buffer(out, capacity);
    return pb_encode(&stream, type, src) ? stream.bytes_written : 0;
}
struct MeshBeaconModule {
    static const RegionInfo *beaconRegionFor(Region r, Preset, bool = true) { return getRegion(r); }
    static void beaconChannelName(const meshtastic_ChannelSettings &, Preset,
                                  char (&)[sizeof(meshtastic_ChannelSettings::name)], bool = true);
    static uint32_t offerFrequencySlot(const BeaconConfig &, uint32_t * = nullptr);
    static bool offerSlotUsable(const BeaconConfig &);
    static void fillOffer(meshtastic_MeshBeacon &, const BeaconConfig &);
};
struct MeshBeaconBroadcastModule : MeshBeaconModule {
    uint8_t payloadCache[meshtastic_MeshBeacon_size]{};
    pb_size_t payloadCacheSize = 0;
    bool payloadCacheDirty = true;
    meshtastic_MeshPacket allocated{};
    std::vector<meshtastic_MeshPacket> sent;
    meshtastic_MeshPacket *allocDataPacket() { allocated = {}; return &allocated; }
    void sendBeaconPacket(const meshtastic_MeshPacket *p) { sent.push_back(*p); }
    static bool setTargetRadioSettings(meshtastic_MeshPacket *, Preset, uint16_t, bool, Region, int16_t, bool) {
        return true;
    }
    void rebuildCache();
    void sendBeacon();
    int32_t runOnce();
};

#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#include "beacon.inc"
}

static unsigned payloads()
{
    unsigned comparisons = 0;
    for (bool text : {false, true})
    for (bool split : {false, true})
    for (bool channel : {false, true})
    for (bool preset : {false, true})
    for (bool region : {false, true})
    for (uint32_t pin : {0u, 1u, 20u, UINT32_MAX}) {
        auto &cfg = donor::moduleConfig.mesh_beacon;
        cfg = {};
        cfg.flags = split ? MESH_BEACON_FLAG_LEGACY_SPLIT : 0;
        if (text) strcpy(cfg.broadcast_message, "field test");
        if (channel) {
            cfg.has_broadcast_offer_channel = true;
            strcpy(cfg.broadcast_offer_channel.name, "LongFast");
            cfg.broadcast_offer_channel.psk.size = 1;
            cfg.broadcast_offer_channel.psk.bytes[0] = 1; // public default
        }
        cfg.has_broadcast_offer_preset = preset;
        if (region) cfg.broadcast_offer_region = meshtastic_Config_LoRaConfig_RegionCode_US;
        cfg.has_broadcast_offer_frequency_slot = pin != 0;
        cfg.broadcast_offer_frequency_slot = pin;
        donor::MeshBeaconBroadcastModule stock;
        stock.sendBeacon();
        mt::stock::BeaconPayloadBatch zt{};
        const bool sent = mt::stock::MeshBeaconBroadcastModule::sendBeacon(cfg, zt, 1);
        assert(sent == !stock.sent.empty() && zt.count == stock.sent.size());
        for (unsigned i = 0; i < zt.count; ++i) {
            const auto &a = zt.packets[i], &b = stock.sent[i].decoded;
            assert(a.portnum == b.portnum && a.payload.size == b.payload.size);
            assert(memcmp(a.payload.bytes, b.payload.bytes, b.payload.size) == 0);
        }
        ++comparisons;
    }
    return comparisons;
}

static unsigned schedule()
{
    unsigned comparisons = 0;
    for (unsigned seconds : {0u, 17u, 3600u, 4001u}) {
        Fixture f;
        auto core = f.service.config;
        // A duty-limited EU region with an unbound utilization meter blocks
        // the first poll. Binding a known-idle clock then releases the gate.
        auto radio = core.radioRequest();
        radio.regionCode = 3;
        radio.channel = 1;
        assert(core.setRadio(radio));
        assert(!f.service.pauseForStorage() && f.service.apply(core));
        assert(f.service.start(false, 0) && f.service.armEndpoint(true));
        meshtastic_ModuleConfig m{};
        m.which_payload_variant = meshtastic_ModuleConfig_mesh_beacon_tag;
        auto &cfg = m.payload_variant.mesh_beacon;
        cfg.flags = MESH_BEACON_FLAG_BROADCAST_ENABLED;
        strcpy(cfg.broadcast_message, "hello");
        // The real settings owner normalizes a nonzero sub-minimum interval.
        cfg.broadcast_interval_secs = seconds;
        assert(!f.edit.save(m, 10, false));
        donor::moduleConfig.mesh_beacon = cfg;
        donor::moduleConfig.mesh_beacon.broadcast_interval_secs = seconds;
        donor::MeshBeaconBroadcastModule stock;
        donor::history.last = 0;
        donor::air.allowed = false;
        uint32_t next = 60000; // same initial wakeup; board startup staggering is independent
        const uint32_t period = donor::beaconIntervalMs();
        assert(period == mt::beacon::BeaconCadence::intervalMs(seconds));
        for (uint32_t now : {59999u, 60000u, 60001u, 60002u, 60000u + period - 1u, 60000u + period}) {
            if (now == 60001) {
                const auto read = [](void *p) { return static_cast<Fixture *>(p)->clock.now(); };
                assert(f.service.airtimeMeter().bind({read, &f}));
                donor::air.allowed = true;
            }
            assert(f.service.moduleAirtimeAllowed() == donor::air.allowed);
            donor::Time::ms = now;
            if (now >= next) next = now + static_cast<uint32_t>(stock.runOnce());
            f.poll(now);
            assert(f.local.size() == stock.sent.size());
            ++comparisons;
        }
        assert(f.local.size() == 1);
        f.send(next - period + 1000);
        assert(f.radio.frames.size() == 1);
        const auto &frame = f.radio.frames.front();
        const auto &expected = stock.sent.front();
        bench::RouteMetadata metadata{};
        assert(f.service.codec.decode(frame.data(), frame.size(), metadata));
        assert(bench::le32(frame.data()) == expected.to && bench::le32(frame.data()+4) == expected.from);
        assert((frame[12]&7u) == expected.hop_limit && ((frame[12]&8u) != 0) == expected.want_ack);
        assert(metadata.portnum == unsigned(expected.decoded.portnum) &&
               metadata.payloadLength == expected.decoded.payload.size);
        assert(!memcmp(metadata.payload, expected.decoded.payload.bytes, metadata.payloadLength));
        ++comparisons;
    }
    return comparisons;
}

int main()
{
    donor::config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    donor::config.lora.use_preset = true;
    donor::config.lora.channel_num = 20;
    printf("%u\n", payloads() + schedule());
}
