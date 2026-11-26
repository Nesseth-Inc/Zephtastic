void precision_cases()
{
    auto make_pos = []() {
        meshtastic_Position p = meshtastic_Position_init_default;
        p.has_latitude_i  = true;
        p.latitude_i      = 123456789;
        p.has_longitude_i = true;
        p.longitude_i     = -123456789;
        p.time            = 0x55667788;
        p.has_altitude=true; p.altitude=-35; p.timestamp=99; p.sats_in_view=12;
        return p;
    };
    // p = 0: whole struct reset to generated default, time retained
    {
        meshtastic_Position pos = make_pos();
        const uint32_t t = pos.time;
        mt::PositionPrecision::apply(pos, 0);
        assert(pos.time == t);                 // time retained
        assert(pos.has_latitude_i == false);   // presence flags -> default
        assert(pos.has_longitude_i == false);
        meshtastic_Position def = meshtastic_Position_init_default;
        pos.time = 0; def.time = 0;
        assert(sameEncodedPosition(pos, def)); // every other field == default
    }
    // p = 1
    {
        meshtastic_Position pos = make_pos();
        mt::PositionPrecision::apply(pos, 1);
        assert(pos.precision_bits == 1);
        assert(pos.latitude_i  == 1073741824);
        assert(pos.longitude_i == -1073741824);
    }
    // p = 15
    {
        meshtastic_Position pos = make_pos();
        mt::PositionPrecision::apply(pos, 15);
        assert(pos.precision_bits == 15);
        assert(pos.latitude_i  == 123404288);
        assert(pos.longitude_i == -123404288);
    }
    // p = 32 / 33 / UINT32_MAX: coords untouched, precision_bits clamped to 32
    for (uint32_t p : {32u, 33u, UINT32_MAX}) {
        meshtastic_Position pos = make_pos();
        mt::PositionPrecision::apply(pos, p);
        assert(pos.precision_bits == 32);
        assert(pos.latitude_i  == 123456789);
        assert(pos.longitude_i == -123456789);
    }
}

static void configurePrimary(mt::Config &c, const uint8_t *bytes, size_t length) {
    mt::PrimaryChannel::Selection key{};
    assert(mt::PrimaryChannel::normalize(bytes, length, key, c.effectivePrimaryName()));
    auto node = c.node();
    node.setKey(key.hash, key.bytes, key.length);
    mt::PrimaryChannel::clear(key);
    c.node(node);
    assert(c.valid() && c.channelConfigured());
}

static void channelPrecisionCases() {
    auto c = initial();
    assert(c.setPrimaryPositionPrecision(32));
    assert(c.setSecondaryChannel(1, channel(2, "Inherited", nullptr, 0, 31)));
    assert(c.positionPrecision(0) == 15 && c.positionPrecision(1) == 15);
    for (uint8_t alias : {1, 2, 128, 255}) {
        assert(c.setSecondaryChannel(2, channel(2, "NotPublic", &alias, 1, 32)));
        assert(c.positionPrecision(2) == 15 && response(c, 2).latitude_i == 123404288);
    }

    uint8_t family[16];
    memcpy(family, bench::MtNodeRecord::PublicKey, sizeof(family));
    family[15] ^= 0x55;
    assert(c.setSecondaryChannel(2, channel(2, "Expanded", family, sizeof(family), 32)));
    assert(c.positionPrecision(2) == 15);
    // ZT also protects a short key that pads to an effective member of the public family.
    assert(c.setSecondaryChannel(2, channel(2, "Padded", family, 15, 32)));
    assert(c.positionPrecision(2) == 15);
    family[0] ^= 0x80;
    assert(c.setSecondaryChannel(2, channel(2, "Custom", family, 15, 32)));
    assert(c.positionPrecision(2) == 32 && response(c, 2).latitude_i == 123456789);

    configurePrimary(c, family, sizeof(family));
    assert(c.positionPrecision(0) == 32 && c.positionPrecision(1) == 31);
    assert(response(c, 1).precision_bits == 31);
    assert(c.setPrimaryPositionPrecision(0));
    uint8_t selected = 77;
    assert(mt::stock::findPositionChannel(mt::PositionChannels(c), selected) && selected == 1);
    assert(c.positionPrecision(0) == 0 && c.positionPrecision(7) == 0);
    for (unsigned invalid : {8u, 255u, 256u, UINT32_MAX}) assert(c.positionPrecision(invalid) == 0);
    c.setPositionSharing(false);
    selected = 77;
    assert(!mt::stock::findPositionChannel(mt::PositionChannels(c), selected) && selected == 77);
    for (unsigned i = 0; i < 8; ++i) assert(c.positionPrecision(i) == 0);
    meshtastic_Channel missing = meshtastic_Channel_init_default;
    missing.role = meshtastic_Channel_Role_PRIMARY;
    missing.has_settings = true;
    missing.settings.module_settings.position_precision = 32;
    assert(mt::stock::getPositionPrecisionForChannel(missing) == 0);
    std::cout << "PASS real channel settings distinguish public aliases, effective keys, inheritance and withheld positions\n";
}

static void eventPrecisionCases() {
    // Synthetic test material only. The opt-in build defines this key as the event PSK.
    uint8_t eventKey[16] = {0xa5, 0x1e, 0x7c};
    uint8_t ordinaryKey[16] = {0xa5, 0x1e, 0x7d};
    auto c = initial();
    configurePrimary(c, eventKey, sizeof(eventKey));
    assert(c.setPrimaryPositionPrecision(32));
    assert(c.setSecondaryChannel(1, channel(2, "Ordinary", nullptr, 0, 32)));
    assert(c.setSecondaryChannel(2, channel(2, "Ordinary", eventKey, sizeof(eventKey), 32)));
    assert(c.setSecondaryChannel(3, channel(2, "Event", ordinaryKey, sizeof(ordinaryKey), 32)));
    uint8_t found = 99;
    assert(mt::stock::findPositionChannel(mt::PositionChannels(c), found));
    assert(found == (TEST_EVENT_BLOCKED ? 3 : 0));
    for (unsigned index : {0u, 1u, 2u}) {
        assert(c.positionPrecision(index) == (TEST_EVENT_BLOCKED ? 0u : 32u));
        uint8_t data[128]{};
        size_t size = 0;
        bool shared = mt::PositionQuery::fixedResponse(c, 105, data, sizeof(data), size, index);
        assert(shared == !TEST_EVENT_BLOCKED && (size != 0) == shared);
    }
    assert(response(c, 3).latitude_i == 123456789 && c.positionPrecision(3) == 32);
    std::cout << "PASS event key and inherited event key policy, independent of channel name\n";
}

static void packetPrecisionCases() {
    using mt::stock::applyPositionPrecisionForChannel;
    for (uint32_t bits = 0; bits <= 33; ++bits) {
        for (int32_t value : {INT32_MIN, -1800000000, -1, 0, 1, 900000000, INT32_MAX}) {
            const auto expected = stock_reference::truncateCoordinate(value, bits);
            assert(mt::stock::truncateCoordinate(value, bits) == expected);
            assert(mt::stock::truncateCoordinate(value, static_cast<uint8_t>(bits)) == expected);
        }
    }
    auto c = initial();
    assert(c.setPrimaryPositionPrecision(32));
    uint8_t privateKey[16] = {0x72};
    assert(c.setSecondaryChannel(1, channel(2, "Private", privateKey, sizeof(privateKey), 32)));
    meshtastic_Position origin = meshtastic_Position_init_default;
    origin.has_latitude_i = origin.has_longitude_i = origin.has_altitude = true;
    origin.latitude_i = 123456789;
    origin.longitude_i = -123456789;
    origin.altitude = -35;
    origin.time = 123;
    origin.timestamp = 78;
    origin.sats_in_view = 11;

    meshtastic_MeshPacket raw = meshtastic_MeshPacket_init_default;
    raw.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    raw.decoded.portnum = meshtastic_PortNum_POSITION_APP;
    raw.id = 7;
    raw.decoded.request_id = 19;
    auto output = pb_ostream_from_buffer(raw.decoded.payload.bytes, sizeof(raw.decoded.payload.bytes));
    assert(pb_encode(&output, meshtastic_Position_fields, &origin));
    raw.decoded.payload.size = output.bytes_written;
    for (uint8_t index : {0, 1, 7, 255}) {
        auto packet = raw;
        assert(applyPositionPrecisionForChannel(packet, mt::PositionChannels(c), index));
        assert(packet.id == 7 && packet.decoded.request_id == 19);
        auto input = pb_istream_from_buffer(packet.decoded.payload.bytes, packet.decoded.payload.size);
        meshtastic_Position actual = meshtastic_Position_init_default;
        assert(pb_decode(&input, meshtastic_Position_fields, &actual) && !input.bytes_left);
        meshtastic_Position expected = meshtastic_Position_init_default;
        if (index < 2) {
            expected = origin;
            expected.precision_bits = index == 0 ? 15 : 32;
            expected.latitude_i = stock_reference::truncateCoordinate(origin.latitude_i, expected.precision_bits);
            expected.longitude_i = stock_reference::truncateCoordinate(origin.longitude_i, expected.precision_bits);
        } else {
            expected.time = origin.time;
        }
        assert(sameEncodedPosition(expected, actual));
    }
    auto malformed = raw;
    malformed.decoded.payload.size = 1;
    malformed.decoded.payload.bytes[0] = 0x80;
    assert(!applyPositionPrecisionForChannel(malformed, mt::PositionChannels(c), 0));
    auto oversized = raw;
    oversized.decoded.payload.size = sizeof(oversized.decoded.payload.bytes) + 1;
    assert(!applyPositionPrecisionForChannel(oversized, mt::PositionChannels(c), 0));
    auto other = raw;
    other.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    assert(applyPositionPrecisionForChannel(other, mt::PositionChannels(c), 7));
    assert(other.decoded.payload.size == raw.decoded.payload.size &&
           !memcmp(other.decoded.payload.bytes, raw.decoded.payload.bytes, raw.decoded.payload.size));
    meshtastic_MeshPacket encrypted = meshtastic_MeshPacket_init_default;
    encrypted.which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    encrypted.encrypted.size = 2;
    encrypted.encrypted.bytes[0] = 0x66;
    encrypted.encrypted.bytes[1] = 0x91;
    assert(applyPositionPrecisionForChannel(encrypted, mt::PositionChannels(c), 7));
    assert(encrypted.which_payload_variant == meshtastic_MeshPacket_encrypted_tag &&
           encrypted.encrypted.size == 2 && encrypted.encrypted.bytes[0] == 0x66 && encrypted.encrypted.bytes[1] == 0x91);
    std::cout << "PASS stock coordinate oracle and real nanopb packet transform, redaction, malformed input and pass-through\n";
}
