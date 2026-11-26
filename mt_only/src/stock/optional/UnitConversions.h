// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d src/modules/Telemetry/UnitConversions.{h,cpp}.
#pragma once
#include <math.h>

namespace mt::stock {
struct UnitConversions {
    static float CelsiusToFahrenheit(float celsius) {
        return (celsius * 9) / 5 + 32;
    }
    static float MetersPerSecondToKnots(float metersPerSecond) {
        return metersPerSecond * 1.94384;
    }
    static float HectoPascalToInchesOfMercury(float hectoPascal) {
        return hectoPascal * 0.029529983071445;
    }
    static float displaySafeFloat(float value) {
        if (!isfinite(value))
            return 0.0f;
        return value < -1e9f ? -1e9f : (value > 1e9f ? 1e9f : value);
    }
};
}
