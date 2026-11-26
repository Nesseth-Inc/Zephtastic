// test_tx_window.cpp
//
// Offline tests for tx_window.h. All inputs synthetic. Expected values
// derived from the pinned stock tests (test/test_airtime/test_main.cpp
// lines 942-1026) and cross-checked by reference.py (stock-faithful,
// second-granular). No compiled-stock differential: the stock target
// build is not available offline; vectors are hand-derived from the
// pinned stock test file.
#include "tx_window.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <cassert>
#ifdef NDEBUG
#error Acceptance requires enabled assertions
#endif

namespace {

using zt::TxMinuteWindow;
using E = TxMinuteWindow::Error;
constexpr std::uint64_t MIN_MS = 60000;

int g_fail = 0;
int g_checks = 0;

void check(bool ok, const char *what, int line) {
  ++g_checks;
  if (!ok) {
    std::printf("FAIL line %d: %s\n", line, what);
    ++g_fail;
  }
}

template <class A, class B>
void eq(const A &a, const B &b, const char *what, int line) {
  check(a == b, what, line);
}

template<class A,class B>
void eq(const zt::Observed<A> &a,const B &b,const char *what,int line){
 check(a.has_value()&&*a==b,what,line);
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define EQ(a, b) eq((a), (b), #a " == " #b, __LINE__)

void near(zt::Observed<float> actual,float expected) {
  CHECK(actual.has_value());
  if(actual) {
    const float delta=*actual>expected?*actual-expected:expected-*actual;
    CHECK(delta<=0.000001f);
  }
}

// T1 stock vector: 36000 ms => 1%; 59 min present / 60 min aged out
// (test_tx_utilization_clear_boundary_is_exactly_sixty_minutes)
void t1_stock_36000_boundary() {
  TxMinuteWindow w;
  CHECK(!w.percentAt(0));  // unavailable before any record
  CHECK(w.record(0, 36000) == E::None);
  EQ(w.percentAt(59 * MIN_MS), 1);  // 36000*100/3600000 = 1.0
  EQ(w.windowSumMs(59 * MIN_MS), 36000ULL);
  EQ(w.percentAt(60 * MIN_MS), 0);  // slot 0 reused at minute 60
  EQ(w.windowSumMs(60 * MIN_MS), 0ULL);
}

// T2 stock vector: only the crossed minutes are cleared
// (test_tx_utilization_clears_only_the_minutes_crossed)
void t2_stock_differing_aged_entries() {
  TxMinuteWindow w;
  CHECK(w.record(0, 1000) == E::None);
  CHECK(w.record(1 * MIN_MS, 2000) == E::None);
  CHECK(w.record(2 * MIN_MS, 3000) == E::None);
  CHECK(w.record(3 * MIN_MS, 4000) == E::None);
  EQ(w.windowSumMs(4 * MIN_MS), 10000ULL);  // all four minutes inside
  near(w.percentAt(4 * MIN_MS),float(10000)/3600000.0f*100.0f);
  EQ(w.windowSumMs(60 * MIN_MS), 9000ULL);  // minute 0's slot reused
  near(w.percentAt(60 * MIN_MS),0.25f);
}

// T3 stock vector: quantisation defect preserved
// (test_tx_utilization_quantisation_error: reported < truth, > 0.95*truth)
void t3_stock_quantisation() {
  TxMinuteWindow w;
  for (std::uint64_t m = 0; m < 60; ++m)
    CHECK(w.record(m * MIN_MS, 1000) == E::None);
  // Read at minute 60: slot 0 (minute 0) cleared -> 59000, not 60000.
  EQ(w.windowSumMs(60 * MIN_MS), 59000ULL);
  near(w.percentAt(60 * MIN_MS),float(59000)/3600000.0f*100.0f);
}

// T4 minute phase boundary: 59999 ms and 60000 ms land in different slots
void t4_minute_phase_boundary() {
  TxMinuteWindow w;
  CHECK(w.record(59999, 30000) == E::None);  // minute 0 -> slot 0
  CHECK(w.record(60000, 30000) == E::None);  // minute 1 -> slot 1
  EQ(w.windowSumMs(119999), 60000ULL);
  near(w.percentAt(119999),float(60000)/3600000.0f*100.0f);
}

// T5 poll-rate independence: frequent 1 ms reads vs one big jump
void t5_frequent_1ms_vs_big_jump() {
  TxMinuteWindow a, b;
  CHECK(a.record(0, 36000) == E::None);
  CHECK(b.record(0, 36000) == E::None);
  for (std::uint64_t t = 1; t <= 3540000; ++t)
    a.advance(t);  // 3.54M 1 ms polls
  EQ(a.percentAt(3540000), 1);
  CHECK(b.advance(3540000) == E::None);  // one big jump
  EQ(b.percentAt(3540000), 1);
  for (std::uint64_t t = 3540001; t <= 3600000; ++t)
    a.advance(t);
  EQ(a.percentAt(3600000), 0);
  CHECK(b.advance(3600000) == E::None);
  EQ(b.percentAt(3600000), 0);
}

// T6 idle reads without TX: unavailable, not zero
void t6_idle_reads_without_tx() {
  TxMinuteWindow w;
  CHECK(!w.percentAt(1000));
  CHECK(w.advance(100000) == E::None);
  CHECK(w.advance(7000000) == E::None);  // 115-min gap: bounded full clear
  CHECK(!w.percentAt(7000000));  // no record: unavailable
  CHECK(w.record(7000000, 36000) == E::None);
  EQ(w.percentAt(7000000), 1);
}

// T7 idle aging with a prior TX: real zero after a >= 60-min gap
void t7_idle_aging_with_tx() {
  TxMinuteWindow w;
  CHECK(w.record(100000, 36000) == E::None);
  EQ(w.percentAt(100000), 1);
  CHECK(w.advance(7000000) == E::None);  // 115-min gap >= 60: whole ring
  EQ(w.percentAt(7000000), 0);  // real zero, available
  EQ(w.windowSumMs(7000000), 0ULL);
}

// T8 bounded clear on a gap >= 1 hour; a 59-min gap keeps the anchor slot
void t8_gap_bounded_clear() {
  TxMinuteWindow w;
  CHECK(w.record(0, 36000) == E::None);
  CHECK(w.advance(3600000) == E::None);  // exactly 60 min
  EQ(w.percentAt(3600000), 0);
  CHECK(w.advance(43200000) == E::None);  // +120 min: still bounded
  EQ(w.percentAt(43200000), 0);
  TxMinuteWindow v;
  CHECK(v.record(0, 36000) == E::None);
  CHECK(v.advance(59 * MIN_MS) == E::None);  // 59 slots cleared, slot 0 kept
  EQ(v.percentAt(59 * MIN_MS), 1);
}

// T9 64-bit uptime across the 32-bit ms boundary (49.7-day wrap)
void t9_64bit_across_32bit_boundary() {
  const std::uint64_t kU32_MAX = 0xFFFFFFFFULL;
  TxMinuteWindow w;
  CHECK(w.record(kU32_MAX - 30000, 36000) == E::None);  // below 2^32
  CHECK(w.advance(kU32_MAX + 30000) == E::None);  // crosses 2^32
  EQ(w.percentAt(kU32_MAX + 30000), 1);
  EQ(w.windowSumMs(kU32_MAX + 30000), 36000ULL);
  const std::uint64_t k2p32 = 1ULL << 32;
  TxMinuteWindow v;
  CHECK(v.record(k2p32 + 1000, 36000) == E::None);
  CHECK(v.advance(k2p32 + 3600000) == E::None);  // 60-min gap: whole ring
  EQ(v.percentAt(k2p32 + 3600000), 0);
}

// T10 non-zero initial uptime (minute 2057, slot 17)
void t10_nonzero_initial_uptime() {
  const std::uint64_t t0 = 123456789;
  TxMinuteWindow w;
  CHECK(w.record(t0, 36000) == E::None);
  EQ(w.percentAt(t0 + 59 * MIN_MS), 1);
  EQ(w.percentAt(t0 + 60 * MIN_MS), 0);
}

// T11 backward clock: explicit rejection, state preserved, no wrap magic
void t11_backward_clock() {
  TxMinuteWindow w;
  CHECK(w.record(100000, 36000) == E::None);
  CHECK(w.record(99999, 1000) == E::BackwardClock);
  EQ(w.lastError(), E::BackwardClock);
  EQ(w.windowSumMs(100000), 36000ULL);  // state unchanged
  CHECK(!w.percentAt(99999));  // rejected: unavailable
  EQ(w.percentAt(100001), 1);  // forward motion recovers
  // A wrapped uint32 ms value is a backward clock, never a small forward
  // delta.
  TxMinuteWindow v;
  CHECK(v.record(0xFFFFFFF0ULL, 36000) == E::None);
  CHECK(v.record(0x10, 1000) == E::BackwardClock);
  EQ(v.windowSumMs(0xFFFFFFF0ULL), 36000ULL);
}

// T12 overflow policy: checked, rejected, no wrap, no silent clamp
void t12_overflow_policy() {
  TxMinuteWindow w;
  CHECK(w.record(0, 3000000000ULL) == E::None);
  CHECK(w.record(0, 3000000000ULL) == E::Overflow);  // 6e9 > 2^32-1
  EQ(w.windowSumMs(0), 3000000000ULL);  // rejected record dropped
  EQ(w.percentAt(0), 100);  // 3e9*100/3600000 = 83333.33 -> clamped to 100
}

// T13 zero, exact 100, and clamp above 100 (stock float would exceed 100)
void t13_zero_max_finite() {
  TxMinuteWindow z;
  CHECK(z.record(0, 1) == E::None);
  CHECK(z.percentAt(0).has_value());
  near(z.percentAt(0),1.0f/3600000.0f*100.0f);
  CHECK(*z.percentAt(0)>0.0f);  // even one ms stays nonzero
  EQ(z.percentAt(60 * MIN_MS),0);  // aged-out real zero
  TxMinuteWindow m;
  CHECK(m.record(0, 60000) == E::None);
  for (std::uint64_t min = 1; min <= 58; ++min)
    CHECK(m.record(min * MIN_MS, 60000) == E::None);
  CHECK(m.record(59 * MIN_MS, 120000) == E::None);  // slot > its period
  // Read at minute 60: slot 0 cleared; 58*60000 + 120000 = 3600000
  EQ(m.windowSumMs(60 * MIN_MS), 3600000ULL);
  EQ(m.percentAt(60 * MIN_MS), 100);
  TxMinuteWindow x;
  CHECK(x.record(0, 60000) == E::None);
  for (std::uint64_t min = 1; min <= 58; ++min)
    CHECK(x.record(min * MIN_MS, 60000) == E::None);
  CHECK(x.record(59 * MIN_MS, 3600000) == E::None);  // cross-boundary
  EQ(x.windowSumMs(60 * MIN_MS), 7080000ULL);
  EQ(x.percentAt(60 * MIN_MS), 100);  // clamped; stock float: 196.67
}

void t14_overflow_does_not_move_clock() {
  TxMinuteWindow w;
  CHECK(w.record(0,std::numeric_limits<std::uint32_t>::max())==E::None);
  const auto anchor=w.anchorMs();
  CHECK(w.record(1,1)==E::Overflow);
  EQ(w.anchorMs(),anchor);
  EQ(w.windowSumMs(0),std::numeric_limits<std::uint32_t>::max());
  CHECK(w.record(MIN_MS,1)==E::None); // a fresh destination bucket fits
}

void t15_unavailable_reads_still_validate_time() {
  TxMinuteWindow w;
  CHECK(!w.percentAt(1000));
  CHECK(w.anchored());EQ(w.anchorMs(),1000ULL);
  CHECK(!w.percentAt(999));
  EQ(w.lastError(),E::BackwardClock);EQ(w.anchorMs(),1000ULL);
  CHECK(w.record(999,1)==E::BackwardClock);
  CHECK(w.record(1000,6000)==E::None);
  near(w.percentAt(1000),6000.0f/3600000.0f*100.0f);
}

}  // namespace

int main() {
  std::printf("sizeof(TxMinuteWindow) = %zu bytes "
              "(60 x uint32 slots + uint64 anchor + flags)\n",
              sizeof(TxMinuteWindow));
  t1_stock_36000_boundary();
  t2_stock_differing_aged_entries();
  t3_stock_quantisation();
  t4_minute_phase_boundary();
  t5_frequent_1ms_vs_big_jump();
  t6_idle_reads_without_tx();
  t7_idle_aging_with_tx();
  t8_gap_bounded_clear();
  t9_64bit_across_32bit_boundary();
  t10_nonzero_initial_uptime();
  t11_backward_clock();
  t12_overflow_policy();
  t13_zero_max_finite();
  t14_overflow_does_not_move_clock();
  t15_unavailable_reads_still_validate_time();
  if (g_fail != 0) {
    std::printf("FAILED: %d/%d checks\n", g_fail, g_checks);
    return 1;
  }
  std::printf("ALL TESTS PASSED (%d checks)\n", g_checks);
  if(g_checks!=279)return 1;
  std::printf("PASS all fifteen counter scenarios and 279 checks\n");
  return 0;
}
