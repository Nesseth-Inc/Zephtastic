// wall_clock_gnss_tests.cpp
// Fixture for mt::WallClock (mt_only/src/WallClock.h): GNSS-like UTC anchor,
// monotonic interpolation, 30-minute correction guard, at() overflow.
#include "WallClock.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>

int main() {
  using mt::WallClock;
  const uint64_t M0 = 1000000u;
  const uint32_t U = 1761638400u; // Synthetic GNSS-like UTC seconds.

  WallClock c;
  assert(c.at(M0) == 0); // unset clock reads zero
  assert(c.set(U, M0) == 0); // initial set with nonzero GNSS-like UTC
  assert(c.at(M0) == U);
  assert(c.at(M0 + 1999u) == U + 1u); // monotonic interpolation floors to seconds
  assert(c.at(M0 + 2000u) == U + 2u);
  assert(c.set(0u, M0) == -EINVAL); // zero set rejected
  assert(c.set(U + 1u, M0 - 1u) == -EINVAL); // backwards monotonic rejected
  assert(c.at(M0 + 1000u) == U + 1u);
  assert(c.set(U + 1u, M0 + 1000u) == 0); // idempotent: at() matches, no re-anchor
  assert(c.set(U + 2u, M0 + 1000u) == -EAGAIN); // differing UTC inside 30-minute guard
  assert(c.set(U + 2000u, M0 + WallClock::CorrectionMs) == 0); // exactly at guard accepted
  assert(c.at(M0 + WallClock::CorrectionMs) == U + 2000u); // anchor actually moved
  assert(c.set(U + 2000u, M0 - 1u) == -EINVAL); // backwards after re-anchor

  WallClock high;
  const uint32_t nearMax = UINT32_MAX - 10u;
  assert(high.set(nearMax, 0u) == 0);
  assert(high.at(10000u) == nearMax + 10u); // last representable second
  assert(high.at(11000u) == 0); // second overflow saturates to zero
  return 0;
}
