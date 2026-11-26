// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace zt::telemetry {
// EnvironmentTelemetryModule constructs sensors in this order. AHT fills
// missing fields even if an earlier sensor already wrote a value.
enum class EnvironmentSource : uint8_t {
  Unspecified, Aht, Declared, BoardAnalog, Spa06, Lark, Bmp085, Bme280, Bme680,
  Bmp280, Dps310, Mcp9808, Lps22hb, Mlx90632, Bmp3xx, Pct2075, Sht, OneWire
};
}
