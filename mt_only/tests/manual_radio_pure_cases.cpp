// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <iterator>
namespace resolution {
using namespace mt;
#include "manual_radio/resolution.inc"
}
namespace power {
using namespace mt;
#include "manual_radio/power.inc"
}
namespace normalization {
using namespace mt;
#include "manual_radio/normalization.inc"
}
namespace frequency {
using namespace mt;
#include "manual_radio/frequency.inc"
}
namespace mode {
using namespace mt;
#include "manual_radio/mode-transition.inc"
}
namespace migration {
using namespace mt;
#include "manual_radio/v13-migration.inc"
}
int main() {
  resolution::AssertM96RadioSettingsResolution();
  puts("PASS preset/manual profile resolution and atomic refusal");
  assert(power::m96_power_layer_assertions());
  puts("PASS regional power, chip boundary and retained legacy defaults");
  normalization::m96_manual_normalization_assertions();
  puts("PASS manual bandwidth SF CR normalization and native allowlist");
  frequency::assertM96USChannelLookupStock();
  puts("PASS US float frequency oracle and atomic refusal");
  mode::modeTransitionAssertions();
  puts("PASS empty-name inherited key preset manual preset transition");
  std::ifstream fixture("v13-config.bin", std::ios::binary);
  assert(fixture.good());
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(fixture)), {});
  migration::legacyConfigAssertions(raw.data(), raw.size());
  puts("PASS actual v13 migration body preservation and CRC refusal");
}
