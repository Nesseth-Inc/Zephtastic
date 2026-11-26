// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/AmbientLightingThread.h
#pragma once

#include <stdint.h>

namespace mt::stock::ambient {

constexpr uint32_t RefreshMs = 30000;

struct Config {
    bool ledState = false;
    uint8_t current = 0, red = 0, green = 0, blue = 0;
};

using SetLighting = int (*)(void *, uint8_t, uint8_t, uint8_t, uint8_t);
enum class Result : uint8_t { Disabled, Unavailable, NotDue, Applied, Failed };

// AmbientLightingThread::runOnce. Board discovery and the Zephyr PWM transfer
// replace the compile-time RGB controller branches. A failed transfer remains
// visible and does not advance the owner's refresh timer.
inline Result runOnce(const Config &config, bool capable, SetLighting setLighting, void *context)
{
    if (!config.ledState)
        return Result::Disabled;
    if (!capable || !setLighting)
        return Result::Unavailable;
    if (setLighting(context, config.current, config.red, config.green, config.blue) != 0)
        return Result::Failed;
    return Result::Applied;
}

} // namespace mt::stock::ambient
