// SPDX-License-Identifier: MIT
// Qwen proposal corrected against ZephCore time conversion and Python UTC.
// Milliseconds are within-minute; retain ms/1000 and reject leap seconds.
#include <stdint.h>
#include <stdio.h>

#include "gnss/GnssUtcTime.h"

namespace {
int failures = 0;
const uint32_t kSentinel = 0xDEADBEEFu;

zt::gnss::UtcFields makeFields(uint8_t cy, uint8_t mo, uint8_t dy,
                               uint8_t hr, uint8_t mi, uint16_t ms) {
  zt::gnss::UtcFields f;
  f.centuryYear = cy; f.month = mo; f.day = dy;
  f.hour = hr; f.minute = mi; f.millisecond = ms;
  return f;
}

void expectOk(const char *name, zt::gnss::UtcFields f, uint32_t want) {
  uint32_t out = kSentinel;
  bool ok = zt::gnss::toUnixSeconds(f, out);
  if (!ok || out != want) {
    printf("FAIL %s: expected ok and %u, got ok=%d out=%u\n",
           name, want, ok ? 1 : 0, out);
    ++failures;
  }
}

void expectInvalid(const char *name, zt::gnss::UtcFields f) {
  uint32_t out = kSentinel;
  bool ok = zt::gnss::toUnixSeconds(f, out);
  if (ok || out != kSentinel) {
    printf("FAIL %s: expected rejection with out unchanged, got ok=%d out=%u\n",
           name, ok ? 1 : 0, out);
    ++failures;
  }
}
}  // namespace

int main() {
  // Exact known values
  expectOk("2000-01-01T00:00:00Z", makeFields(0, 1, 1, 0, 0, 0), 946684800u);
  expectOk("2024-02-29T00:00:00Z", makeFields(24, 2, 29, 0, 0, 0), 1709164800u);
  expectOk("2099-12-31T23:59:59Z", makeFields(99, 12, 31, 23, 59, 59999), 4102444799u);
  // Valid leap day in 2000 (century leap year): 2000-02-29T00:00:00Z
  expectOk("2000-02-29T00:00:00Z", makeFields(0, 2, 29, 0, 0, 0), 951782400u);
  // Month/day boundaries
  expectOk("2000-01-31T00:00:00Z", makeFields(0, 1, 31, 0, 0, 0), 949276800u);
  expectOk("2000-03-01T00:00:00Z", makeFields(0, 3, 1, 0, 0, 0), 951868800u);
  expectOk("2001-02-28T00:00:00Z", makeFields(1, 2, 28, 0, 0, 0), 983318400u);
  // Milliseconds within-minute preserve their whole seconds.
  expectOk("ms=0", makeFields(0, 1, 1, 0, 0, 0), 946684800u);
  expectOk("ms=999", makeFields(0, 1, 1, 0, 0, 999), 946684800u);
  expectOk("ms=1000", makeFields(0, 1, 1, 0, 0, 1000), 946684801u);
  expectOk("ms=59999", makeFields(0, 1, 1, 0, 0, 59999), 946684859u);
  // Invalid: 2001-02-29 (2001 is not a leap year)
  expectInvalid("2001-02-29", makeFields(1, 2, 29, 0, 0, 0));
  // Invalid days
  expectInvalid("2000-02-30", makeFields(0, 2, 30, 0, 0, 0));
  expectInvalid("2000-04-31", makeFields(0, 4, 31, 0, 0, 0));
  expectInvalid("day=0", makeFields(0, 1, 0, 0, 0, 0));
  expectInvalid("day=32", makeFields(0, 1, 32, 0, 0, 0));
  // Out-of-range fields
  expectInvalid("year=100", makeFields(100, 1, 1, 0, 0, 0));
  expectInvalid("month=0", makeFields(0, 0, 1, 0, 0, 0));
  expectInvalid("month=13", makeFields(0, 13, 1, 0, 0, 0));
  expectInvalid("hour=24", makeFields(0, 1, 1, 24, 0, 0));
  expectInvalid("minute=60", makeFields(0, 1, 1, 0, 60, 0));
  expectInvalid("ms=60000", makeFields(0, 1, 1, 0, 0, 60000));
  expectInvalid("ms=65535", makeFields(0, 1, 1, 0, 0, 65535));
  if (failures != 0) {
    printf("gnss_utc_time_fixture: %d check(s) failed\n", failures);
    return 1;
  }
  printf("gnss_utc_time_fixture: all checks passed\n");
  return 0;
}
