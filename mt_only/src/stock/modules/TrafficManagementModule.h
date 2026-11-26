// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/TrafficManagementModule.cpp
#pragma once
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"
#include <pb_decode.h>

namespace mt::stock {

struct TrafficManagementModule {
    // The native owner selects a fixed per-source slot and expires it against the full monotonic clock.
    static bool isRateLimited(uint8_t &rateCount, uint8_t &rateTime, uint8_t nowRateTick,
                              uint32_t windowSeconds, uint32_t threshold)
    {
        if (windowSeconds == 0 || threshold == 0)
            return false;
        const uint32_t ticks = windowSeconds / 300;
        const uint8_t windowTicks = ticks < 1 ? 1 : ticks > 15 ? 15 : static_cast<uint8_t>(ticks);
        const bool windowExpired = rateCount == 0 ||
            (static_cast<uint8_t>(nowRateTick - rateTime) & 0x0F) >= windowTicks;
        if (windowExpired) {
            rateTime = nowRateTick & 0x0F;
            rateCount = 1;
            return false;
        }

        if (rateCount < 0x3F)
            ++rateCount;
        if (threshold > 60)
            threshold = 60;
        return rateCount > threshold;
    }

    static bool shouldDropUnknown(uint8_t &unknownCount, uint8_t &unknownTime, uint8_t nowUnknownTick,
                                  uint32_t threshold)
    {
        if (threshold == 0)
            return false;
        static constexpr uint8_t kUnknownWindowTicks = 5;
        const bool windowExpired = unknownCount == 0 ||
            (static_cast<uint8_t>(nowUnknownTick - unknownTime) & 0x0F) >= kUnknownWindowTicks;
        if (windowExpired) {
            unknownTime = nowUnknownTick & 0x0F;
            unknownCount = 0;
        }
        if (unknownCount < 0x3F)
            ++unknownCount;
        if (threshold > 60)
            threshold = 60;
        return unknownCount > threshold;
    }

    static uint8_t sanitizePositionPrecision(uint8_t precision) {
        return precision > 0 && precision <= 32 ? precision : 19;
    }

    static uint8_t computePositionFingerprint(int32_t lat_truncated, int32_t lon_truncated, uint8_t precision)
    {
        precision = sanitizePositionPrecision(precision);
        uint8_t bitsToTake = (precision < 4) ? precision : 4;
        uint8_t shift = 32 - precision;
        uint8_t latBits = (static_cast<uint32_t>(lat_truncated) >> shift) & ((1u << bitsToTake) - 1);
        uint8_t lonBits = (static_cast<uint32_t>(lon_truncated) >> shift) & ((1u << bitsToTake) - 1);
        const uint8_t fingerprint = static_cast<uint8_t>((latBits << 4) | lonBits);
        return fingerprint ? fingerprint : 0xFF;
    }

    // Service separately owns encrypted/unknown admission, signed NodeInfo and direct responses.
    // This is the decoded transit portion of the upstream handler.
    // Port of TrafficManagementModule::alterReceived. ZT relays the sender's
    // original wire payload, so suppress a frame when forwarding it would
    // disclose more than this channel permits. Local reception is independent.
    static bool allowsPositionRelay(const uint8_t *payload, size_t size, unsigned channelPrecision)
    {
        if (channelPrecision == 32) return true;
        if (!payload || channelPrecision > 32) return false;
        meshtastic_Position pos = meshtastic_Position_init_default;
        auto input = pb_istream_from_buffer(payload, size);
        if (!pb_decode(&input, meshtastic_Position_fields, &pos) || input.bytes_left) return false;
        if (!pos.has_latitude_i && !pos.has_longitude_i) return true;

        const uint32_t packetPrecision = pos.precision_bits > 0 ? pos.precision_bits : 32u;
        if (!channelPrecision || packetPrecision > channelPrecision) return false;
        // Verify the transmitted values match the stock midpoint at the
        // declared precision. The declaration cannot redact extra low bits.
        const uint32_t mask = UINT32_MAX >> packetPrecision;
        const uint32_t midpoint = uint32_t(1) << (31 - packetPrecision);
        return (!pos.has_latitude_i || (uint32_t(pos.latitude_i) & mask) == midpoint) &&
               (!pos.has_longitude_i || (uint32_t(pos.longitude_i) & mask) == midpoint);
    }

    template <class DropPosition, class LimitRate>
    static bool handleReceived(uint32_t from, uint32_t to, uint32_t self, uint32_t port,
                               const uint8_t *payload, size_t length, bool wellKnownChannel,
                               const meshtastic_ModuleConfig_TrafficManagementConfig &cfg,
                               DropPosition shouldDropPosition, LimitRate isRateLimited)
    {
        if (from != self && to != self) {
            if (wellKnownChannel && port == meshtastic_PortNum_POSITION_APP && cfg.position_min_interval_secs && payload) {
                meshtastic_Position pos = meshtastic_Position_init_zero;
                auto stream = pb_istream_from_buffer(payload, length);
                if (pb_decode(&stream, meshtastic_Position_fields, &pos) && !stream.bytes_left &&
                    pos.has_latitude_i && pos.has_longitude_i && shouldDropPosition(pos))
                    return true;
            }
            if (cfg.rate_limit_window_secs && cfg.rate_limit_max_packets) {
                if (port != meshtastic_PortNum_ROUTING_APP && port != meshtastic_PortNum_ADMIN_APP) {
                    if (isRateLimited())
                        return true;
                }
            }
        }
        return false;
    }
};
}
