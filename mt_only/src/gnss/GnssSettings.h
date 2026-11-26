// SPDX-License-Identifier: GPL-3.0-only
// GnssSettings.h -- fixed-width 32-byte GNSS settings codec (config v16 body).
// Pure C++17, header-only. Serializes/deserializes GnssSettings to a 32-byte
// little-endian wire body. Field-by-field (no struct memcpy, no padding
// dependence). Runtime default resolution and 64-bit timing are out of scope.

#ifndef MT_GNSS_SETTINGS_H
#define MT_GNSS_SETTINGS_H

#include <stdint.h>
#include <stddef.h>

namespace mt {

struct GnssSettings {
    bool enabled = false;
    bool smart = false;
    uint32_t positionFlags = 0;
    uint32_t updateSecs = 0;
    uint32_t attemptSecs = 0;
    uint32_t broadcastSecs = 0;
    uint32_t smartMinDistance = 0;
    uint32_t smartMinInterval = 0;

    static constexpr size_t Size = 32;
    static constexpr uint32_t SupportedFlags = 0x3ff;

    // Rejects positionFlags with bits outside SupportedFlags. All other
    // uint32 seconds/distance fields are unrestricted.
    bool valid() const {
        return (positionFlags & ~SupportedFlags) == 0u;
    }

    // Serializes into out. Requires nonnull out and len == Size; otherwise
    // returns false and writes nothing. On success writes all 32 bytes,
    // zeroing every reserved byte.
    bool encode(uint8_t *out, size_t len) const {
        if (out == nullptr || len != Size || !valid()) return false;
        out[0] = static_cast<uint8_t>((enabled ? 1u : 0u) | (smart ? 2u : 0u));
        out[1] = 0;
        writeLe32(out + 2, positionFlags);
        writeLe32(out + 6, updateSecs);
        writeLe32(out + 10, attemptSecs);
        writeLe32(out + 14, broadcastSecs);
        writeLe32(out + 18, smartMinDistance);
        writeLe32(out + 22, smartMinInterval);
        out[26] = 0;
        out[27] = 0;
        out[28] = 0;
        out[29] = 0;
        out[30] = 0;
        out[31] = 0;
        return true;
    }

    // Deserializes from in into dst. Requires nonnull in and len == Size,
    // byte0 limited to enabled/smart bits, byte1 zero, and bytes 26..31 zero.
    // Any failure returns false and leaves dst unchanged.
    static bool decode(const uint8_t *in, size_t len, GnssSettings &dst) {
        if (in == nullptr || len != Size) return false;
        if ((in[0] & ~0x3u) != 0u) return false;
        if (in[1] != 0u) return false;
        for (size_t i = 26; i < 32; ++i) {
            if (in[i] != 0u) return false;
        }
        GnssSettings c;
        c.enabled = (in[0] & 0x1u) != 0u;
        c.smart = (in[0] & 0x2u) != 0u;
        c.positionFlags = readLe32(in + 2);
        c.updateSecs = readLe32(in + 6);
        c.attemptSecs = readLe32(in + 10);
        c.broadcastSecs = readLe32(in + 14);
        c.smartMinDistance = readLe32(in + 18);
        c.smartMinInterval = readLe32(in + 22);
        if (!c.valid()) return false;
        dst = c;
        return true;
    }

private:
    static void writeLe32(uint8_t *p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xffu);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
    }
    static uint32_t readLe32(const uint8_t *p) {
        return static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    }
};

}  // namespace mt

#endif  // MT_GNSS_SETTINGS_H
