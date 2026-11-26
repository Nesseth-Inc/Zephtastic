// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "meshtastic/atak.pb.h"
#include "meshtastic/mesh.pb.h"
#include "pb_encode.h"
#ifdef PB_VALIDATE_UTF8
#include "pb_common.h"
#endif

namespace mt {
namespace tak_pli {

// Source-backed projection only. Caller owns role selection, freshness,
// scheduling, channel selection, normal MeshPacket allocation, and routing.
static constexpr size_t PayloadLimit = 233; // bounded product contract
static constexpr size_t EncodedLimit = PayloadLimit - 1;
static constexpr uint8_t RawProtobufMarker = 0xff;
static constexpr meshtastic_CotType NodeBroadcastCotType =
    meshtastic_CotType_CotType_a_f_G_U_C;
static constexpr meshtastic_Team DefaultTeam = meshtastic_Team_Cyan;
static constexpr meshtastic_MemberRole DefaultRole =
    meshtastic_MemberRole_TeamMember;

struct Inputs {
    const meshtastic_Position *position;
    const char *ownerLongName;
    uint32_t nodeNumber;
    uint8_t batteryPercent;
    bool fixedPosition;
    bool hasTakConfig;
    meshtastic_Team configuredTeam;
    meshtastic_MemberRole configuredRole;
};

enum class Result : uint8_t {
    Ok,
    InvalidInput,
    EncodeFailed,
    OutputTooSmall
};

inline bool project(const Inputs &in, meshtastic_TAKPacketV2 &out)
{
    if (!in.position || !in.ownerLongName)
        return false;

    out = meshtastic_TAKPacketV2_init_zero;
    out.cot_type_id = NodeBroadcastCotType;
    out.team = in.hasTakConfig && in.configuredTeam != meshtastic_Team_Unspecifed_Color
        ? in.configuredTeam : DefaultTeam;
    out.role = in.hasTakConfig && in.configuredRole != meshtastic_MemberRole_Unspecifed
        ? in.configuredRole : DefaultRole;

    // Match allocAtakPli: values are copied from the runtime local-position
    // snapshot without consulting Position protobuf presence flags.
    out.latitude_i = in.position->latitude_i;
    out.longitude_i = in.position->longitude_i;
    out.altitude = in.position->altitude_hae;
    out.speed = in.position->ground_speed;
    int32_t course = static_cast<int32_t>(in.position->ground_track / 1000u);
    if (course < 0)
        course = 0;
    else if (course > 36000)
        course = 36000;
    out.course = static_cast<uint16_t>(course);
    out.battery = in.batteryPercent;

    if (in.fixedPosition ||
        in.position->location_source == meshtastic_Position_LocSource_LOC_MANUAL) {
        out.how = meshtastic_CotHow_CotHow_h_e;
        out.geo_src = meshtastic_GeoPointSource_GeoPointSource_USER;
        out.alt_src = meshtastic_GeoPointSource_GeoPointSource_USER;
    } else {
        out.how = meshtastic_CotHow_CotHow_m_g;
        out.geo_src = meshtastic_GeoPointSource_GeoPointSource_GPS;
        out.alt_src = meshtastic_GeoPointSource_GeoPointSource_GPS;
    }

    strncpy(out.callsign, in.ownerLongName, sizeof(out.callsign) - 1);
    out.callsign[sizeof(out.callsign) - 1] = '\0';
    strncpy(out.device_callsign, in.ownerLongName, sizeof(out.device_callsign) - 1);
    out.device_callsign[sizeof(out.device_callsign) - 1] = '\0';
    snprintf(out.uid, sizeof(out.uid), "!%08x", static_cast<unsigned>(in.nodeNumber));
    return true;
}

namespace detail {
// Only project()'s stock node-position fields are serialized here. Referencing
// the complete TAK descriptor also retains every unrelated TAK union variant.
// Native differential tests compare these bytes with the generated descriptor.
struct PliWriter {
    uint8_t *data; size_t capacity; size_t size = 0;
    bool byte(uint8_t value) { if(size == capacity) return false; data[size++] = value; return true; }
    bool varint(uint32_t value) {
        do { const uint8_t part = uint8_t(value & 127u); value >>= 7;
             if(!byte(uint8_t(part | (value ? 128u : 0u)))) return false;
        } while(value);
        return true;
    }
    bool scalar(unsigned tag, uint32_t value) {
        return !value || (byte(uint8_t(tag << 3)) && varint(value));
    }
    bool fixed(unsigned tag, int32_t value) {
        const uint32_t bits = uint32_t(value);
        return !value || (byte(uint8_t((tag << 3) | 5u)) && byte(uint8_t(bits)) &&
            byte(uint8_t(bits >> 8)) && byte(uint8_t(bits >> 16)) && byte(uint8_t(bits >> 24)));
    }
    bool sint(unsigned tag, int32_t value) {
        const uint32_t bits = uint32_t(value);
        return scalar(tag, (bits << 1) ^ (0u - (bits >> 31)));
    }
    template<size_t N> bool text(unsigned tag, const char (&value)[N]) {
        size_t length = 0; while(length < N && value[length]) ++length;
        if(length == N) return false;
#ifdef PB_VALIDATE_UTF8
        if(!pb_validate_utf8(value)) return false;
#endif
        if(!length) return true;
        if(!byte(uint8_t((tag << 3) | 2u)) || !varint(uint32_t(length)) ||
           length > capacity - size) return false;
        memcpy(data + size, value, length); size += length; return true;
    }
};
inline bool encodeProjected(const meshtastic_TAKPacketV2 &packet,
                            uint8_t *output, size_t capacity, size_t &size) {
    PliWriter out{output, capacity};
    const bool ok = out.scalar(1, uint32_t(packet.cot_type_id)) &&
        out.scalar(2, uint32_t(packet.how)) && out.text(3, packet.callsign) &&
        out.scalar(4, uint32_t(packet.team)) && out.scalar(5, uint32_t(packet.role)) &&
        out.fixed(6, packet.latitude_i) && out.fixed(7, packet.longitude_i) &&
        out.sint(8, packet.altitude) && out.scalar(9, packet.speed) &&
        out.scalar(10, packet.course) && out.scalar(11, packet.battery) &&
        out.scalar(12, uint32_t(packet.geo_src)) && out.scalar(13, uint32_t(packet.alt_src)) &&
        out.text(14, packet.uid) && out.text(15, packet.device_callsign);
    size = ok ? out.size : 0; return ok;
}
} // namespace detail

// Emits the stock raw-protobuf prefix plus protobuf into a caller-owned,
// fixed-size buffer. A staging array makes failure transactional.
[[gnu::noinline]] inline Result encode(const Inputs &in, uint8_t *output, size_t outputCapacity,
                     size_t &written)
{
    written = 0;
    if (!output || outputCapacity == 0)
        return Result::InvalidInput;
    if (outputCapacity > PayloadLimit)
        outputCapacity = PayloadLimit;
    if (outputCapacity < 2)
        return Result::OutputTooSmall;

    meshtastic_TAKPacketV2 packet = meshtastic_TAKPacketV2_init_zero;
    if (!project(in, packet))
        return Result::InvalidInput;

    uint8_t encoded[EncodedLimit];
    size_t encodedSize = 0;
    if (!detail::encodeProjected(packet, encoded, sizeof(encoded), encodedSize) ||
        encodedSize == 0)
        return Result::EncodeFailed;
    if (encodedSize + 1 > outputCapacity)
        return Result::OutputTooSmall;

    output[0] = RawProtobufMarker;
    memcpy(output + 1, encoded, encodedSize);
    written = encodedSize + 1;
    return Result::Ok;
}

} // namespace tak_pli
} // namespace mt

