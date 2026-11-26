// SPDX-License-Identifier: GPL-3.0-only
#include "relay_fixture_provider.h"

// Uses the actual Service RX/queue/poll path and its capture-only Radio fixture.
static void neighborRelayCases()
{
    enum Case { Licensed, OmittedSender, ShortSender, Disabled, Encrypted, Signed, Strict, Mqtt, BadOuter, DuplicateOuter, BadInner, DuplicateInner, UnknownInner, NoHop, Query };
    for (auto which : {Licensed, OmittedSender, ShortSender, Disabled, Encrypted, Signed, Strict, Mqtt, BadOuter, DuplicateOuter, BadInner, DuplicateInner, UnknownInner, NoHop, Query}) {
        constexpr uint32_t self = 0x12345678, peer = 0x24680135, id = 931;
        Radio radio;
        mt::test::KnownChannelFixtureClock clock;
        mt::Service service(radio, true, self, 127, true, false);
        clock.bind(service);
        RelayFixtureProvider provider(self);
        assert(service.bindPrivateRuntime(provider));
        mt::Config cfg;
        cfg.bytes[4] = 1;
        auto node = cfg.node(); node.setPublic(); cfg.node(node);
        cfg.setTxEnabled(true);
        assert(cfg.setDeviceRole(meshtastic_Config_DeviceConfig_Role_ROUTER, 0));
        if (which != Encrypted) assert(cfg.enableLicensedOperation());
        if (which == Strict) assert(cfg.setPacketSignaturePolicy(2));
        mt::ModuleSettings modules;
        meshtastic_ModuleConfig wire{};
        wire.which_payload_variant = meshtastic_ModuleConfig_neighbor_info_tag;
        wire.payload_variant.neighbor_info.enabled = which != Disabled;
        assert(modules.set(wire));
        assert(service.bindNeighborSnapshot([](void *, meshtastic_NeighborInfo &) { return false; }, &modules,
                                            [](void *m) { return bool(static_cast<mt::ModuleSettings *>(m)->bytes[9]); }));
        assert(service.apply(cfg));
        assert(service.start(false, 0));
        assert(service.armEndpoint(true) && service.relaying());
        meshtastic_NeighborInfo info{};
        info.node_id = peer; info.last_sent_by_id = which == OmittedSender ? 0 : which == ShortSender ? 7 : peer; info.node_broadcast_interval_secs = 900;
        info.neighbors_count = 1; info.neighbors[0].node_id = 219; info.neighbors[0].snr = -1.25f;
        meshtastic_Data data{};
        data.portnum = meshtastic_PortNum_NEIGHBORINFO_APP; data.has_bitfield = true;
        if (which == Query) data.want_response = true;
        auto nested = pb_ostream_from_buffer(data.payload.bytes, sizeof(data.payload.bytes));
        assert(pb_encode(&nested, meshtastic_NeighborInfo_fields, &info));
        data.payload.size = nested.bytes_written;
        if (which == BadInner) data.payload.bytes[data.payload.size++] = 0xff;
        if (which == DuplicateInner) {
            // The first field is node_id (one tag and five varint bytes). A repeated identical field
            // decodes normally under nanopb but cannot gain rewrite authority.
            memcpy(data.payload.bytes + data.payload.size, data.payload.bytes, 6);
            data.payload.size += 6;
        }
        if (which == UnknownInner) {
            data.payload.bytes[data.payload.size++] = 0x78; data.payload.bytes[data.payload.size++] = 1;
        }
        if (which == Signed) { data.xeddsa_signature.size = 64; memset(data.xeddsa_signature.bytes, 0x42, 64); }
        uint8_t encoded[239]{}, frame[255]{};
        auto outer = pb_ostream_from_buffer(encoded, sizeof(encoded));
        assert(pb_encode(&outer, meshtastic_Data_fields, &data));
        size_t size = outer.bytes_written;
        if (which == DuplicateOuter) { encoded[size++] = 0x08; encoded[size++] = 71; }
        if (which == BadOuter) { encoded[size++] = 0x78; encoded[size++] = 0x01; }
        mt::PrimaryChannel::Selection key;
        assert(cfg.channelSelection(0, key));
        if (which == Encrypted) {
            bench::MtCodec sender;
            assert(sender.setKey(key.hash, key.bytes, key.length));
            size_t written = 0;
            assert(sender.encodeZeroHopData(peer, UINT32_MAX, id, encoded, size, frame, sizeof(frame), written));
            size = written;
        } else {
            assert(key.licensedPlaintext);
            mt::Config::put32(frame, UINT32_MAX); mt::Config::put32(frame + 4, peer); mt::Config::put32(frame + 8, id);
            frame[13] = key.hash;
            memcpy(frame + 16, encoded, size); size += 16;
        }
        frame[12] = which == NoHop ? 3 << 5 : (3 << 5) | 3;
        if (which == Mqtt) frame[12] |= 0x10;
        frame[15] = uint8_t(peer);
        mt::PrimaryChannel::clear(key);
        bool accepted = false;
        service.rx(frame, size, -5, 100, 0, true, &accepted);
        const bool eligible = which == Licensed || which == OmittedSender || which == ShortSender || which == Encrypted ||
                              which == BadOuter || which == DuplicateOuter || which == BadInner || which == DuplicateInner || which == UnknownInner;
        // Zero-hop frames can enter history without scheduling any RF.
        if (which != NoHop) assert(accepted == eligible);
        for (uint32_t now = 200; now < 10000 && !radio.sends; now += 100) service.poll(now, 0);
        assert(radio.sends == unsigned(eligible));
        if (eligible) {
            assert(radio.sent.size() == size && !memcmp(frame + 16,radio.sent.data()+16,size-16));
            assert(!memcmp(frame, radio.sent.data(), 12) && radio.sent[13] == frame[13]);
            assert((radio.sent[12] & 7) == 2 && radio.sent[15] == uint8_t(self));
        }
    }
}
