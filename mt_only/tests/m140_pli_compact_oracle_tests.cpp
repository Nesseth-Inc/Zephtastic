#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/modules/TakPliPayload.h"

namespace {
using mt::tak_pli::EncodedLimit;
using mt::tak_pli::Inputs;
using mt::tak_pli::PayloadLimit;
using mt::tak_pli::Result;

// Frozen full-schema oracle: project with the production projection, then
// serialize the complete generated nanopb descriptor exactly as the old
// TakPliPayload::encode did before the compact field writer.
Result generatedOracle(const Inputs &in, uint8_t *output, size_t capacity,
                       size_t &written)
{
    written = 0;
    if (!output || capacity == 0)
        return Result::InvalidInput;
    if (capacity > PayloadLimit)
        capacity = PayloadLimit;
    if (capacity < 2)
        return Result::OutputTooSmall;

    meshtastic_TAKPacketV2 packet = meshtastic_TAKPacketV2_init_zero;
    if (!mt::tak_pli::project(in, packet))
        return Result::InvalidInput;

    uint8_t encoded[EncodedLimit];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    if (!pb_encode(&stream, &meshtastic_TAKPacketV2_msg, &packet) ||
        stream.bytes_written == 0)
        return Result::EncodeFailed;
    if (stream.bytes_written + 1 > capacity)
        return Result::OutputTooSmall;

    output[0] = mt::tak_pli::RawProtobufMarker;
    memcpy(output + 1, encoded, stream.bytes_written);
    written = stream.bytes_written + 1;
    return Result::Ok;
}

unsigned comparisons = 0;

void compare(const Inputs &in, size_t capacity)
{
    uint8_t expected[PayloadLimit];
    uint8_t actual[PayloadLimit];
    memset(expected, 0xa5, sizeof(expected));
    memset(actual, 0xa5, sizeof(actual));
    size_t expectedSize = 0x1234;
    size_t actualSize = 0x1234;
    const Result expectedResult = generatedOracle(
        in, expected, capacity, expectedSize);
    const Result actualResult = mt::tak_pli::encode(
        in, actual, capacity, actualSize);
    assert(actualResult == expectedResult);
    assert(actualSize == expectedSize);
    if (expectedResult == Result::Ok) {
        assert(expectedSize > 1 && expectedSize <= PayloadLimit);
        assert(memcmp(actual, expected, expectedSize) == 0);
        assert(actual[0] == 0xff);
        for (size_t i = expectedSize; i < sizeof(actual); ++i)
            assert(actual[i] == 0xa5);
    } else {
        assert(actualSize == 0);
        assert(expectedSize == 0);
        assert(memcmp(actual, expected, sizeof(actual)) == 0);
    }
    ++comparisons;
}

Inputs makeInputs(meshtastic_Position &position, const char *name,
                  uint32_t node, uint8_t battery, bool fixed,
                  bool configured, int team, int role)
{
    Inputs in{};
    in.position = &position;
    in.ownerLongName = name;
    in.nodeNumber = node;
    in.batteryPercent = battery;
    in.fixedPosition = fixed;
    in.hasTakConfig = configured;
    in.configuredTeam = static_cast<meshtastic_Team>(team);
    in.configuredRole = static_cast<meshtastic_MemberRole>(role);
    return in;
}

void compareCapacityBoundaries(const Inputs &in)
{
    compare(in, 0);
    compare(in, 1);
    compare(in, 2);
    compare(in, PayloadLimit);
    compare(in, PayloadLimit + 1); // encoder clamps to its public limit

    uint8_t complete[PayloadLimit];
    size_t completeSize = 0;
    const Result fullResult = mt::tak_pli::encode(
        in, complete, sizeof(complete), completeSize);
    if (fullResult != Result::Ok) {
        assert(completeSize == 0);
        return; // Long projected strings may exceed the stock 232-byte bound.
    }
    assert(completeSize > 1);
    compare(in, completeSize - 1);
    compare(in, completeSize);
}
} // namespace

int main()
{
    const int32_t latitudes[] = {0, 1, -1, 900000000, -900000000,
                                 INT32_MAX, INT32_MIN};
    const int32_t longitudes[] = {0, 1, -1, 1800000000, -1800000000,
                                  INT32_MAX, INT32_MIN};
    const int32_t altitudes[] = {0, 1, -1, INT32_MAX, INT32_MIN};
    const uint32_t speeds[] = {0, 1, 65535, UINT32_MAX};
    const uint32_t tracks[] = {0, 999, 1000, 35999999, 36000000,
                               36000999, UINT32_MAX};

    char name[120];
    for (unsigned length = 0; length < sizeof(name); ++length) {
        memset(name, 'A' + (length % 26), length);
        name[length] = '\0';
        meshtastic_Position position = meshtastic_Position_init_zero;
        position.latitude_i = latitudes[length % (sizeof(latitudes) / sizeof(latitudes[0]))];
        position.longitude_i = longitudes[(length * 3) % (sizeof(longitudes) / sizeof(longitudes[0]))];
        position.altitude_hae = altitudes[(length * 2) % (sizeof(altitudes) / sizeof(altitudes[0]))];
        position.ground_speed = speeds[length % (sizeof(speeds) / sizeof(speeds[0]))];
        position.ground_track = tracks[length % (sizeof(tracks) / sizeof(tracks[0]))];
        position.location_source = length & 1
            ? meshtastic_Position_LocSource_LOC_MANUAL
            : meshtastic_Position_LocSource_LOC_INTERNAL;
        const Inputs in = makeInputs(position, name,
            length % 2 ? UINT32_MAX : 0, uint8_t(length % 101),
            (length % 3) == 0, (length % 2) != 0,
            int(length % 15), int(length % 9));
        compare(in, PayloadLimit);
        if (length == 0 || length == 31 || length == 32 || length == 119)
            compareCapacityBoundaries(in);
    }

    // Exhaust all generated, valid team/role enum values, including explicit
    // Unspecified values whose projection intentionally selects stock defaults.
    for (int team = int(meshtastic_Team_Unspecifed_Color);
         team <= int(meshtastic_Team_Brown); ++team) {
        for (int role = int(meshtastic_MemberRole_Unspecifed);
             role <= int(meshtastic_MemberRole_K9); ++role) {
            meshtastic_Position position = meshtastic_Position_init_zero;
            position.latitude_i = team % 2 ? -900000000 : 900000000;
            position.longitude_i = role % 2 ? -1800000000 : 1800000000;
            position.altitude_hae = (team + role) % 2 ? INT32_MIN : INT32_MAX;
            position.ground_speed = UINT32_MAX - uint32_t(role);
            position.ground_track = tracks[(team + role) %
                (sizeof(tracks) / sizeof(tracks[0]))];
            position.location_source = role % 2
                ? meshtastic_Position_LocSource_LOC_MANUAL
                : meshtastic_Position_LocSource_LOC_INTERNAL;
            char enumName[24];
            snprintf(enumName, sizeof(enumName), "T%d-R%d", team, role);
            compare(makeInputs(position, enumName, UINT32_MAX, 100,
                role % 2 != 0, true, team, role), PayloadLimit);
        }
    }

    // Exercise each battery value and both fixed-position/source branches.
    for (unsigned battery = 0; battery <= 100; ++battery) {
        meshtastic_Position position = meshtastic_Position_init_zero;
        position.latitude_i = int32_t(battery) - 50;
        position.longitude_i = 50 - int32_t(battery);
        position.altitude_hae = int32_t(battery) * -1000;
        position.ground_speed = battery * 1000;
        position.ground_track = tracks[battery %
            (sizeof(tracks) / sizeof(tracks[0]))];
        position.location_source = battery % 2
            ? meshtastic_Position_LocSource_LOC_MANUAL
            : meshtastic_Position_LocSource_LOC_INTERNAL;
        const Inputs in = makeInputs(position, "battery-boundary", battery,
            uint8_t(battery), battery % 2 != 0, true,
            meshtastic_Team_Cyan, meshtastic_MemberRole_TeamMember);
        compare(in, PayloadLimit);
        if (battery == 0 || battery == 100)
            compareCapacityBoundaries(in);
    }

    // Full coordinates and numeric edges, with a non-empty and empty owner.
    for (unsigned i = 0; i < 20; ++i) {
        meshtastic_Position position = meshtastic_Position_init_zero;
        position.latitude_i = i & 1 ? INT32_MIN : INT32_MAX;
        position.longitude_i = i & 1 ? INT32_MAX : INT32_MIN;
        position.altitude_hae = i & 1 ? INT32_MAX : INT32_MIN;
        position.ground_speed = UINT32_MAX;
        position.ground_track = tracks[i % (sizeof(tracks) / sizeof(tracks[0]))];
        position.location_source = i & 1
            ? meshtastic_Position_LocSource_LOC_MANUAL
            : meshtastic_Position_LocSource_LOC_INTERNAL;
        const Inputs in = makeInputs(position, i & 1 ? "edge" : "", i,
            uint8_t(i % 101), i & 1, i & 1,
            meshtastic_Team_Unspecifed_Color,
            meshtastic_MemberRole_Unspecifed);
        compare(in, PayloadLimit);
    }

    // Nanopb optionally validates protobuf strings. Compare both builds so a
    // compact writer retains that supported configuration as well as the
    // product's normal macro-off path.
    meshtastic_Position textPosition = meshtastic_Position_init_zero;
    textPosition.latitude_i = 0;
    textPosition.longitude_i = 0;
    textPosition.location_source = meshtastic_Position_LocSource_LOC_INTERNAL;
    const char *utf8Names[] = {
        "Unit-\xc2\xa2", "Unit-\xe2\x98\x83", "Unit-\xf0\x9f\x8c\x90",
        "ASCII"
    };
    for (const char *text : utf8Names)
        compare(makeInputs(textPosition, text, UINT32_MAX, 0, false, false,
            meshtastic_Team_Unspecifed_Color,
            meshtastic_MemberRole_Unspecifed), PayloadLimit);

    const char *invalidUtf8Names[] = {
        "\xff", "\x80", "\xc2", "\xc0\xaf", "\xed\xa0\x80",
        "\xf4\x90\x80\x80"
    };
    for (const char *text : invalidUtf8Names)
        compare(makeInputs(textPosition, text, 0, 100, true, true,
            meshtastic_Team_Cyan, meshtastic_MemberRole_TeamMember),
            PayloadLimit);

    // Truncation may split a multi-byte sequence at the generated field cap;
    // old generated encoding is the oracle for that edge too.
    char longUtf8[73]{};
    for (unsigned i = 0; i < 24; ++i) {
        longUtf8[3 * i] = char(0xe2);
        longUtf8[3 * i + 1] = char(0x98);
        longUtf8[3 * i + 2] = char(0x83);
    }
    compare(makeInputs(textPosition, longUtf8, UINT32_MAX, 100, false, true,
        meshtastic_Team_Blue, meshtastic_MemberRole_HQ), PayloadLimit);

    // Reach the actual 119-byte callsign cap with a three-byte UTF-8
    // sequence beginning at byte 118. strncpy retains only its lead byte,
    // so the generated and compact writers must agree on rejection when
    // UTF-8 validation is enabled and on bytes/status otherwise. The complete
    // name is longer than the protobuf field and does not end at the cap.
    char splitAtCallsignCap[122]{};
    memset(splitAtCallsignCap, 'N', 118);
    splitAtCallsignCap[118] = char(0xe2);
    splitAtCallsignCap[119] = char(0x98);
    splitAtCallsignCap[120] = char(0x83);
    splitAtCallsignCap[121] = '\0';
    const Inputs splitBoundary = makeInputs(textPosition, splitAtCallsignCap,
        UINT32_MAX, 100, false, true, meshtastic_Team_Blue,
        meshtastic_MemberRole_HQ);
    compare(splitBoundary, PayloadLimit);
    // This maximally truncated value is larger than the whole Data payload
    // budget because both stock callsign fields carry the same 119 bytes.
    // Confirm capacity failures remain transactional at every public boundary.
    compare(splitBoundary, 0);
    compare(splitBoundary, 1);
    compare(splitBoundary, PayloadLimit - 1);
    compare(splitBoundary, PayloadLimit);

    // A complete multibyte name with sufficient room below the field cap supplies
    // the sufficient-capacity and one-byte-short boundary comparisons.
    char nearCapUtf8[74]{};
    memset(nearCapUtf8, 'V', 70);
    nearCapUtf8[70] = char(0xe2);
    nearCapUtf8[71] = char(0x98);
    nearCapUtf8[72] = char(0x83);
    nearCapUtf8[73] = '\0';
    const Inputs nearCap = makeInputs(textPosition, nearCapUtf8, UINT32_MAX,
        100, false, true, meshtastic_Team_Blue, meshtastic_MemberRole_HQ);
    uint8_t nearPayload[PayloadLimit];
    size_t nearPayloadSize = 0;
    assert(mt::tak_pli::encode(nearCap, nearPayload, sizeof(nearPayload),
        nearPayloadSize) == Result::Ok);
    assert(nearPayloadSize > 1 && nearPayloadSize < PayloadLimit);
    compareCapacityBoundaries(nearCap);

    assert(comparisons >= 410);
    printf("PASS compact PLI oracle: %u byte/status/capacity comparisons against full generated TAKPacketV2 nanopb encoding\n",
           comparisons);
    return 0;
}
