#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "pb_decode.h"
#include "../src/modules/TakPliPayload.h"

static mt::tak_pli::Inputs inputsFor(meshtastic_Position &pos, const char *name)
{
    mt::tak_pli::Inputs in{};
    in.position = &pos;
    in.ownerLongName = name;
    in.nodeNumber = 0x1234abcd;
    in.batteryPercent = 73;
    in.configuredTeam = meshtastic_Team_Unspecifed_Color;
    in.configuredRole = meshtastic_MemberRole_Unspecifed;
    return in;
}

int main()
{
    meshtastic_Position pos = meshtastic_Position_init_zero;
    pos.latitude_i = 123456789;
    pos.longitude_i = -987654321;
    pos.altitude_hae = 432;
    pos.ground_speed = 17;
    pos.ground_track = 1234567; // degrees * 1e5 -> 1234 degrees * 100, clamped
    pos.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;

    auto in = inputsFor(pos, "Tactical Unit");
    meshtastic_TAKPacketV2 projected = meshtastic_TAKPacketV2_init_zero;
    assert(mt::tak_pli::project(in, projected));
    assert(projected.cot_type_id == meshtastic_CotType_CotType_a_f_G_U_C);
    assert(projected.team == meshtastic_Team_Cyan);
    assert(projected.role == meshtastic_MemberRole_TeamMember);
    assert(projected.latitude_i == pos.latitude_i);
    assert(projected.longitude_i == pos.longitude_i);
    assert(projected.altitude == pos.altitude_hae);
    assert(projected.speed == pos.ground_speed);
    assert(projected.course == 1234);
    assert(projected.battery == 73);
    assert(projected.how == meshtastic_CotHow_CotHow_m_g);
    assert(projected.geo_src == meshtastic_GeoPointSource_GeoPointSource_GPS);
    assert(strcmp(projected.callsign, "Tactical Unit") == 0);
    assert(strcmp(projected.device_callsign, "Tactical Unit") == 0);
    assert(strcmp(projected.uid, "!1234abcd") == 0);

    in.hasTakConfig = true;
    // Explicitly unspecified configured enums retain the stock fallback.
    assert(mt::tak_pli::project(in, projected));
    assert(projected.team == meshtastic_Team_Cyan);
    assert(projected.role == meshtastic_MemberRole_TeamMember);
    in.configuredTeam = meshtastic_Team_Red;
    in.configuredRole = meshtastic_MemberRole_Medic;
    assert(mt::tak_pli::project(in, projected));
    assert(projected.team == meshtastic_Team_Red);
    assert(projected.role == meshtastic_MemberRole_Medic);

    in.fixedPosition = true;
    pos.ground_track = 999999999u;
    assert(mt::tak_pli::project(in, projected));
    assert(projected.course == 36000);
    assert(projected.how == meshtastic_CotHow_CotHow_h_e);
    assert(projected.geo_src == meshtastic_GeoPointSource_GeoPointSource_USER);
    assert(projected.alt_src == meshtastic_GeoPointSource_GeoPointSource_USER);

    in.fixedPosition = false;
    pos.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
    assert(mt::tak_pli::project(in, projected));
    assert(projected.how == meshtastic_CotHow_CotHow_h_e);

    // Projection is all-or-nothing for absent inputs.
    auto bad = in;
    bad.ownerLongName = nullptr;
    memset(&projected, 0x5a, sizeof(projected));
    int32_t beforeLat = projected.latitude_i;
    uint8_t beforeBattery = projected.battery;
    assert(!mt::tak_pli::project(bad, projected));
    assert(projected.latitude_i == beforeLat && projected.battery == beforeBattery);

    pos.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
    in = inputsFor(pos, "Tactical Unit");
    uint8_t payload[mt::tak_pli::PayloadLimit];
    memset(payload, 0xa5, sizeof(payload));
    size_t length = 99;
    assert(mt::tak_pli::encode(in, payload, sizeof(payload), length) == mt::tak_pli::Result::Ok);
    assert(length > 1 && length <= sizeof(payload));
    assert(payload[0] == mt::tak_pli::RawProtobufMarker);
    meshtastic_TAKPacketV2 decoded = meshtastic_TAKPacketV2_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload + 1, length - 1);
    assert(pb_decode(&stream, &meshtastic_TAKPacketV2_msg, &decoded));
    assert(decoded.latitude_i == pos.latitude_i);
    assert(decoded.longitude_i == pos.longitude_i);
    assert(decoded.altitude == pos.altitude_hae);
    assert(decoded.course == 36000);
    assert(decoded.team == meshtastic_Team_Cyan);
    assert(strcmp(decoded.uid, "!1234abcd") == 0);

    // A buffer that cannot hold any protobuf leaves destination untouched.
    uint8_t tiny[2] = {0x5a, 0xc3};
    length = 42;
    assert(mt::tak_pli::encode(in, tiny, sizeof(tiny), length) ==
           mt::tak_pli::Result::OutputTooSmall);
    assert(length == 0 && tiny[0] == 0x5a && tiny[1] == 0xc3);

    // Null output and malformed required inputs fail without claiming bytes.
    length = 42;
    assert(mt::tak_pli::encode(in, nullptr, 0, length) == mt::tak_pli::Result::InvalidInput);
    assert(length == 0);

    // Long but valid stock owner string is bounded to the TAK field.
    char longName[180];
    memset(longName, 'N', sizeof(longName));
    longName[sizeof(longName) - 1] = '\0';
    in = inputsFor(pos, longName);
    assert(mt::tak_pli::project(in, projected));
    assert(projected.callsign[sizeof(projected.callsign) - 1] == '\0');
    assert(strlen(projected.callsign) == sizeof(projected.callsign) - 1);

    // Two maximum-sized callsigns cannot fit the stock 232-byte protobuf
    // allowance. Encoding fails transactionally and reports no payload.
    uint8_t full[mt::tak_pli::PayloadLimit];
    memset(full, 0x6d, sizeof(full));
    const uint8_t first = full[0];
    length = 77;
    assert(mt::tak_pli::encode(in, full, sizeof(full), length) ==
           mt::tak_pli::Result::EncodeFailed);
    assert(length == 0 && full[0] == first && full[sizeof(full) - 1] == 0x6d);
    return 0;
}

