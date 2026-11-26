// SPDX-License-Identifier: GPL-3.0-only
#ifndef MT_UI_RADIO_VIEW_H
#define MT_UI_RADIO_VIEW_H
#include <stddef.h>
#include <stdint.h>
namespace mt::ui {
struct RadioView {
    bool loaded = false;
    bool enabled = false;
    bool regionUS = false;
    char channel[12]{};
    char modem[20]{};
    uint32_t frequencyHz = 0;
    int32_t powerDbm = 0;
    uint32_t bandwidthHz = 0;
    uint8_t spreadingFactor = 0, codingDenominator = 0;
};
namespace detail {
inline bool copyName(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (;;) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (c == 0) {
            if (i == 0) return false;
            dst[i] = 0;
            return true;
        }
        if (i + 1 >= cap) return false;
        if (c < 0x20u || c > 0x7eu) return false;
        dst[i] = static_cast<char>(c);
        ++i;
    }
}
}
inline bool set(RadioView& out, bool loaded, bool enabled, const char* channel,
                const char* modem, uint32_t frequencyHz, int32_t powerDbm) {
    RadioView staged{};
    staged.loaded = loaded;
    if (!loaded) { out = staged; return true; }
    staged.enabled = enabled;
    if (!enabled) { out = staged; return true; }
    if (channel == nullptr || modem == nullptr) return false;
    if (frequencyHz == 0) return false;
    if (powerDbm < -128 || powerDbm > 127) return false;
    if (!detail::copyName(staged.channel, sizeof staged.channel, channel)) return false;
    if (!detail::copyName(staged.modem, sizeof staged.modem, modem)) return false;
    staged.frequencyHz = frequencyHz;
    staged.powerDbm = powerDbm;
    out = staged;
    return true;
}
}
#endif
