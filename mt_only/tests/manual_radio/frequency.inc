// SPDX-License-Identifier: GPL-3.0-only
// Root-adapted Qwen proposal; independently executed by the host suite.
void assertM96USChannelLookupStock() {
  // Literal US PROFILE_STD stock oracle: 902..928 MHz, spacing=0, padding=0, overrideSlot=0.
  struct StockSel { bool ok; uint16_t slotCount, slot; uint32_t frequencyHz; };
  auto stockOracle = [](float bandwidthKHz, uint32_t channelNum, const char *name,
                        float overrideMHz, float offsetMHz) {
    StockSel r{false, 0, 0, 0};
    if (!name || !std::isfinite(bandwidthKHz) || bandwidthKHz <= 0 ||
        !std::isfinite(overrideMHz) || !std::isfinite(offsetMHz)) return r;
    const float freqStart = 902.0f, freqEnd = 928.0f, spacing = 0.0f, padding = 0.0f;
    const float width = spacing + 2.0f * padding + bandwidthKHz / 1000.0f;
    const double countD = std::round(double((freqEnd - freqStart + spacing) / width));
    if (countD < 1 || countD > 65535) return r;
    const uint32_t count = uint32_t(countD);
    uint32_t hash = 5381;
    for (const char *p = name; *p; ++p) hash = hash * 33u + uint8_t(*p);
    const uint32_t slot = (channelNum && channelNum <= count) ? channelNum - 1 : hash % count;
    const float base = freqStart + bandwidthKHz / 2000.0f + padding + float(slot) * width;
    const float centre = (overrideMHz != 0.0f ? overrideMHz : base) + offsetMHz;
    if (!std::isfinite(centre) || centre < 902.0f || centre > 928.0f) return r;
    r.ok = true;
    r.slotCount = uint16_t(count);
    r.slot = uint16_t(overrideMHz != 0.0f ? UINT16_MAX : slot);
    r.frequencyHz = uint32_t(std::llround(double(centre) * 1'000'000.0));
    return r;
  };
  int fail = 0;
  auto check = [&](bool c, const char *w) {
    if (!c) { ++fail; std::fprintf(stderr, "M96 USChannel assertion failed: %s\n", w); }
  };
  const char *names[2] = {"Custom", "LongFast"};
  const float bw[10] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
  // 1) Differential: literal oracle vs helper; both names, all 10 bandwidths, channel 1 and 0.
  for (const char *name : names)
    for (float b : bw)
      for (uint32_t ch : {1u, 0u}) {
        USChannelSelection got{};
        bool ok = USChannel::lookupStock(b, ch, name, 0.0f, 0.0f, got);
        StockSel exp = stockOracle(b, ch, name, 0.0f, 0.0f);
        check(ok == exp.ok, "ok");
        if (ok) {
          check(got.slotCount == exp.slotCount, "slotCount");
          check(got.slot == exp.slot, "slot");
          check(got.frequencyHz == exp.frequencyHz, "frequencyHz");
        }
      }
  // 2) Nonzero override precedence (base ignored), then offset added after.
  {
    USChannelSelection got{};
    bool ok = USChannel::lookupStock(125.0f, 0u, "Custom", 915.0f, 5.0f, got);
    StockSel exp = stockOracle(125.0f, 0u, "Custom", 915.0f, 5.0f);
    check(ok && exp.ok, "override ok");
    check(ok && got.slot == UINT16_MAX, "override slot sentinel");
    check(ok && got.frequencyHz == 920000000u, "override centre = 920.0 MHz exactly");
    check(ok && got.frequencyHz == exp.frequencyHz, "override+offset Hz matches oracle");
  }
  // 3) Override+offset outside 902..928 rejected without mutating sentinel output.
  {
    USChannelSelection sent{0xFFFF, 0xFFFE, 0xDEADBEEFu,0xa5u};
    USChannelSelection got = sent;
    bool ok = USChannel::lookupStock(125.0f, 0u, "Custom", 925.0f, 10.0f, got);
    check(!ok, "out-of-range override rejected");
    check(got.slotCount == sent.slotCount && got.slot == sent.slot &&
          got.frequencyHz == sent.frequencyHz && got.regionCode == sent.regionCode, "sentinel unmutated");
  }
  // 4) Atomic rejections: NaN/inf bandwidth, NaN override/offset, null name, nonpositive bandwidth.
  {
    USChannelSelection sent{0xFFFF, 0xFFFE, 0xDEADBEEFu,0xa5u};
    const float nanF = std::numeric_limits<float>::quiet_NaN();
    const float infF = std::numeric_limits<float>::infinity();
    struct Case { float bw; uint32_t ch; const char *name; float ov; float off; };
    const Case cases[] = {
      {nanF, 1u, "Custom", 0.0f, 0.0f},
      {infF, 1u, "Custom", 0.0f, 0.0f},
      {125.0f, 1u, "Custom", nanF, 0.0f},
      {125.0f, 1u, "Custom", 0.0f, nanF},
      {125.0f, 1u, nullptr, 0.0f, 0.0f},
      {0.0f, 1u, "Custom", 0.0f, 0.0f},
      {-5.0f, 1u, "Custom", 0.0f, 0.0f},
    };
    for (const Case &c : cases) {
      USChannelSelection got = sent;
      bool ok = USChannel::lookupStock(c.bw, c.ch, c.name, c.ov, c.off, got);
      check(!ok, "rejection");
      check(got.slotCount == sent.slotCount && got.slot == sent.slot &&
            got.frequencyHz == sent.frequencyHz && got.regionCode == sent.regionCode, "atomic (sentinel intact)");
    }
  }
  assert(fail == 0);
}
