#pragma once
#include <cstdint>

namespace mt {
class AgcMaintenance {
    static constexpr uint32_t Minute = 60000;
    static constexpr uint32_t Day = 24 * 60 * Minute;
    uint32_t sampled_ = 0;
    uint32_t successful_ = 0;
    bool pending_ = false;

    bool needed(uint32_t now, uint32_t good) const {
        return !good || now-good >= Minute || !successful_ || now-successful_ >= Day;
    }
public:
    void poll(uint32_t now, uint32_t good, bool idle) {
        if (now-sampled_ < Minute) return;
        sampled_ = now;
        pending_ = idle && needed(now, good);
    }
    bool take(uint32_t now, uint32_t good) {
        const bool due = pending_ && needed(now, good);
        pending_ = false;
        return due;
    }
    void succeeded(uint32_t now) { successful_ = now ? now : 1; }
};
}
