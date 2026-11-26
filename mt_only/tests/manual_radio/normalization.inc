// SPDX-License-Identifier: GPL-3.0-only
// Root-adapted Qwen proposal; independently executed by the host suite.
void m96_manual_normalization_assertions() {
  // (1) Every raw byte 0..255 through bandwidthKHz, stock table:
  //     0 -> 250.0f; known codes -> literal kHz; other bytes -> numeric kHz.
  const auto expectedKHz = [](uint32_t raw) -> float {
    switch (raw) {
      case 0:   return 250.0f;
      case 8:   return 7.8f;
      case 10:  return 10.4f;
      case 16:  return 15.6f;
      case 21:  return 20.8f;
      case 31:  return 31.25f;
      case 42:  return 41.7f;
      case 62:  return 62.5f;
      case 200: return 203.125f;
      default:  return float(raw);
    }
  };
  for (uint32_t raw = 0; raw <= 255; ++raw) {
    assert(ManualLoRa::bandwidthKHz(raw) == expectedKHz(raw));
  }

  // (2) All 10 native widths succeed in resolve and yield their Hz.
  //     raw bandwidth code -> native center/bandwidth Hz (resolve allowlist).
  const uint32_t nativeRaw[10] = {8, 10, 16, 21, 31, 42, 62, 125, 250, 500};
  const uint32_t nativeHz[10]  = {7800, 10400, 15600, 20800, 31250,
                                   41700, 62500, 125000, 250000, 500000};
  for (int i = 0; i < 10; ++i) {
    LoRaPresets::Profile out{};
    const bool ok = ManualLoRa::resolve(nativeRaw[i], 11u, 5u, out);
    assert(ok);
    // Profile is aggregate-initialized in production as {hz,hz,sf,cr};
    // compare against a locally built expected aggregate (no field names).
    const LoRaPresets::Profile expected{nativeHz[i], nativeHz[i],
                                        11u,
                                        5u};
    assert(out.bandwidthHz == expected.bandwidthHz && out.wideBandwidthHz == expected.wideBandwidthHz &&
           out.spreadingFactor == expected.spreadingFactor && out.codingDenominator == expected.codingDenominator);
  }

  // (3) SF raw-byte normalization: 5..12 pass through, else default 11.
  const auto expectedSF = [](uint32_t raw) -> uint8_t {
    const uint8_t b = uint8_t(raw);
    return (b >= 5 && b <= 12) ? b : 11;
  };
  for (uint32_t raw = 0; raw <= 255; ++raw) {
    assert(ManualLoRa::spreadingFactor(raw) == expectedSF(raw));
  }
  // CR raw-byte normalization: 4..8 pass through, else default 5.
  const auto expectedCR = [](uint32_t raw) -> uint8_t {
    const uint8_t b = uint8_t(raw);
    return (b >= 4 && b <= 8) ? b : 5;
  };
  for (uint32_t raw = 0; raw <= 255; ++raw) {
    assert(ManualLoRa::codingDenominator(raw) == expectedCR(raw));
  }

  // (4) CR=4 is rejected for each native width; sentinel Profile left unchanged.
  for (int i = 0; i < 10; ++i) {
    const LoRaPresets::Profile sentinel{0x11111111u, 0x22222222u, 0x33u, 0x44u};
    LoRaPresets::Profile out = sentinel;
    const bool ok = ManualLoRa::resolve(nativeRaw[i], 11u, 4u, out);
    assert(!ok);
    assert(out.bandwidthHz == sentinel.bandwidthHz && out.wideBandwidthHz == sentinel.wideBandwidthHz &&
           out.spreadingFactor == sentinel.spreadingFactor && out.codingDenominator == sentinel.codingDenominator);
  }
}
