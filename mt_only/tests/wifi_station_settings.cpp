#include "network/WifiStationSettings.h"
#include "Config.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

namespace {
using mt::network::WifiStationSettings;
static const uint8_t kZero[108] = {0};

// Group 1: exact default record vector and API surface.
void default_record() {
  static_assert(WifiStationSettings::Size == 108u, "record is 108 bytes");
  WifiStationSettings rec{};
  assert(rec.bytes[0] == 'Z' && rec.bytes[1] == 'W' && rec.bytes[2] == 'F' &&
         rec.bytes[3] == '1');
  assert(memcmp(rec.bytes + 4, kZero + 4, 102) == 0);
  assert(rec.bytes[106] == 0x8A && rec.bytes[107] == 0xED && rec.valid() &&
         !rec.enabled() && rec.ssidLength() == 0u && rec.pskLength() == 0u);
  assert(mt::Config::checksum(rec.bytes, 106u) == 0xED8A);
}

// Group 2: set("A","B") yields the exact configured vector.
void configured_record() {
  WifiStationSettings rec{};
  rec.set(true, "A", 1u, "B", 1u);
  assert(rec.valid() && rec.enabled() && rec.ssidLength() == 1u &&
         rec.pskLength() == 1u);
  assert(rec.bytes[4] == 0x01 && rec.bytes[5] == 0x01 && rec.bytes[6] == 0x01 &&
         rec.bytes[7] == 0x00 && rec.bytes[8] == 'A' && rec.bytes[41] == 'B');
  assert(memcmp(rec.bytes + 9, kZero + 9, 32) == 0);
  assert(memcmp(rec.bytes + 42, kZero + 42, 64) == 0);
  assert(rec.bytes[106] == 0x34 && rec.bytes[107] == 0xB6);
  assert(mt::Config::checksum(rec.bytes, 106u) == 0xB634);
  assert(mt::Config::checksum(reinterpret_cast<const uint8_t*>("123456789"),
                              9u) == 0x29B1);
}

// Group 3: disable preserves credentials; re-enable keeps them; empty PSK is valid.
void disable_preserves_credentials() {
  WifiStationSettings rec{};
  rec.set(true, "A", 1u, "B", 1u);
  rec.setEnabled(false);
  assert(rec.valid() && !rec.enabled() && rec.ssidLength() == 1u &&
         rec.pskLength() == 1u && rec.bytes[8] == 'A' && rec.bytes[41] == 'B');
  rec.setEnabled(true);
  assert(rec.valid() && rec.enabled() && rec.ssidLength() == 1u &&
         rec.pskLength() == 1u && rec.bytes[8] == 'A' && rec.bytes[41] == 'B');
  WifiStationSettings open{};
  open.set(true, "A", 1u, "", 0u);
  assert(open.valid() && open.enabled() && open.ssidLength() == 1u &&
         open.pskLength() == 0u && open.bytes[6] == 0x00 && open.bytes[41] == 0x00);
}

// Group 4: refusal paths preserve every byte; byte7=1 with a recomputed valid
// CRC is rejected by both setters.
void refusal_paths() {
  WifiStationSettings rec{};
  uint8_t base[WifiStationSettings::Size];
  memcpy(base, rec.bytes, sizeof(base));
  rec.set(true, "", 0u, "B", 1u);
  assert(memcmp(rec.bytes, base, sizeof(base)) == 0 && !rec.enabled());
  rec.set(true, "A\0B", 3u, "B", 1u);
  assert(memcmp(rec.bytes, base, sizeof(base)) == 0 && !rec.enabled());
  WifiStationSettings bad{};
  bad.set(true, "A", 1u, "B", 1u);
  bad.bytes[7] = 0x01;
  const uint16_t crc = mt::Config::checksum(bad.bytes, 106u);
  bad.bytes[106] = static_cast<uint8_t>(crc & 0xFFu);
  bad.bytes[107] = static_cast<uint8_t>(crc >> 8);
  uint8_t snap[WifiStationSettings::Size];
  memcpy(snap, bad.bytes, sizeof(snap));
  bad.setEnabled(false);
  assert(memcmp(bad.bytes, snap, sizeof(snap)) == 0);
  bad.set(true, "A", 1u, "B", 1u);
  assert(memcmp(bad.bytes, snap, sizeof(snap)) == 0);
}
}  // namespace

int main() {
  default_record();
  configured_record();
  disable_preserves_credentials();
  refusal_paths();
  return 0;
}
