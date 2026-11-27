// SPDX-License-Identifier: GPL-3.0-only
#include "stock/modules/MeshBeaconModule.h"
#include "modules/MeshBeaconReceivedCache.h"
#include "modules/BeaconExcursion.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

namespace donor {
using NodeNum = uint32_t;
struct { bool has_mesh_beacon; meshtastic_ModuleConfig_MeshBeaconConfig mesh_beacon; } moduleConfig{};
constexpr auto RTCQualityFromNet = 2;
uint32_t getValidTime(int) { return 1234; }
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#include "listen-flag.inc"
struct MeshBeaconListenerModule {
#include "offer.inc"
    BeaconOffer lastReceivedOffer{};
    unsigned notifications = 0;
    void notifyObservers(const meshtastic_MeshPacket *) { ++notifications; }
    bool wantPacket(const meshtastic_MeshPacket *);
    bool handleReceivedProtobuf(const meshtastic_MeshPacket &, meshtastic_MeshBeacon *);
};
struct RadioInterface {};
struct RadioTxHook { enum PreTxAction {PRETX_SEND, PRETX_DROP, PRETX_DEFER}; };
struct MeshBeaconModule_TargetRadioSettings { int channelHash = -1; };
struct MeshBeaconModule {
    inline static bool target, retune, cleared, restored;
    inline static MeshBeaconModule_TargetRadioSettings settings{};
    static bool beaconTxConfigInvalid(const meshtastic_MeshPacket *) { return false; }
    static const auto *getTargetRadioSettings(const meshtastic_MeshPacket *) { return target ? &settings : nullptr; }
    static bool hasTargetRadioSettings(const meshtastic_MeshPacket *) { return target; }
    static void clearTargetRadioSettings(const meshtastic_MeshPacket *) { target = false; cleared = true; }
    static bool reconfigureForBeaconTX(RadioInterface *, const meshtastic_MeshPacket *p) {
        if (!p) { assert(cleared); restored = true; }
        return p && target && retune;
    }
};
struct MeshBeaconTxHook : RadioTxHook {
    PreTxAction beforeTransmit(RadioInterface *, meshtastic_MeshPacket *);
    bool holdsRadio(const meshtastic_MeshPacket *);
    void packetReleased(RadioInterface *, const meshtastic_MeshPacket *);
};
#include "methods.inc"
} // namespace donor

static unsigned listener() {
    unsigned cases = 0;
    for (bool configured : {false, true}) for (bool enabled : {false, true})
        for (auto port : {meshtastic_PortNum_MESH_BEACON_APP, meshtastic_PortNum_TEXT_MESSAGE_APP})
            for (unsigned mask = 0; mask < 32; ++mask) {
                donor::MeshBeaconListenerModule stock{};
                donor::moduleConfig.has_mesh_beacon = configured;
                donor::moduleConfig.mesh_beacon.flags = enabled ? MESH_BEACON_FLAG_LISTEN_ENABLED : 0;
                meshtastic_MeshPacket p{}; p.from = 102; p.decoded.portnum = port;
                const bool want = stock.wantPacket(&p);
                assert(want == mt::stock::MeshBeaconListenerModule::wantPacket(configured && enabled, port));
                if (!want) { ++cases; continue; }
                meshtastic_MeshBeacon offer{};
                if (mask & 1) strcpy(offer.message, "test");
                if (mask & 2) { offer.has_offer_channel = true; strcpy(offer.offer_channel.name, "Local"); }
                if (mask & 4) offer.offer_region = meshtastic_Config_LoRaConfig_RegionCode_US;
                if (mask & 8) { offer.has_offer_preset = true; offer.offer_preset = meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST; }
                if (mask & 16) { offer.has_offer_frequency_slot = true; offer.offer_frequency_slot = 3; }
                stock.handleReceivedProtobuf(p, &offer);
                auto input = offer;
                meshtastic_MeshBeacon cached{};
                const bool kept = mt::stock::MeshBeaconListenerModule::handleReceivedProtobuf(&input, [&](const auto &v) { cached = v; return true; });
                const auto &s = stock.lastReceivedOffer;
                assert(kept == s.valid);
                if (kept) {
                    assert(!cached.message[0] && s.sender == p.from);
                    assert(cached.has_offer_channel == s.has_channel && cached.offer_region == s.region);
                    if (s.has_channel) assert(!strcmp(cached.offer_channel.name, s.channel.name));
                    assert(cached.offer_preset == s.preset && cached.has_offer_frequency_slot == s.has_frequency_slot);
                    assert(cached.offer_frequency_slot == s.frequency_slot);
                }
                ++cases;
            }
    return cases;
}

static unsigned txHook() {
    using E = mt::modules::BeaconExcursion;
    using K = E::ActionKind;
    using S = donor::MeshBeaconModule;
    unsigned cases = 0;
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        E zt;
        E::RadioProfile home{};
        home.slot = 1;
        E::Request r{};
        r.packetId = 42; r.target = home;
        S::target = scenario != 0;
        S::retune = r.requiresRetune = scenario == 2;
        S::cleared = S::restored = false;
        if (r.requiresRetune) r.target.slot = 2;
        if (S::target) assert(zt.enqueue(r));
        donor::MeshBeaconTxHook stock;
        meshtastic_MeshPacket p{}; p.id = r.packetId;
        const auto decision = stock.beforeTransmit(nullptr, &p);
        const auto action = zt.beforeTransmit(p.id, home, 0, 0);
        assert((decision == donor::RadioTxHook::PRETX_DEFER) == (action.kind == K::ConfigureTarget));
        assert(zt.holdsRadio() == stock.holdsRadio(&p));
        if (!S::target) { assert(action.kind == K::SendNormally); ++cases; continue; }
        if (r.requiresRetune) {
            assert(zt.beginConfigure(p.id, 0, 0).kind == K::ConfigureTarget);
            assert(zt.targetConfigured(p.id, true, 0, 0).kind == K::StartTransmission);
        } else assert(action.kind == K::StartTransmission);
        assert(zt.beginTransmit(p.id, 0, 0).kind == K::StartTransmission);
        stock.packetReleased(nullptr, &p);
        const auto complete = zt.transmitCompleted(p.id, 0, 0);
        if (complete.kind == K::RestoreHome) {
            assert(complete.profile.sameRadio(home));
            assert(zt.restoreCompleted(true, 0, 0).kind == K::None);
        } else assert(complete.kind == K::None);
        assert(S::restored && zt.holdsRadio() == stock.holdsRadio(&p) && !zt.holdsRadio());
        ++cases;
    }
    return cases;
}

int main() { printf("%u %u\n", listener(), txHook()); }
