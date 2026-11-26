// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../stock/modules/TrafficManagementModule.h"

namespace mt::traffic {
class StockPerSourceRateWindow {
public:
    bool allow(uint8_t nowRateTick, uint32_t windowSeconds, uint32_t configuredPacketCap) {
        return !stock::TrafficManagementModule::isRateLimited(count_, startTick_, nowRateTick, windowSeconds, configuredPacketCap);
    }
    void reset() { count_ = startTick_ = 0; }
private:
    uint8_t startTick_ = 0;
    uint8_t count_ = 0;
};
}
