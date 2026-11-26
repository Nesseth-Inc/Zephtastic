// SPDX-License-Identifier: GPL-3.0-only
// Root-reviewed literal policy boundary vectors; host-only.
#include <cstdio>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include "RadioTiming.h"
#include "TxAdmission.h"
#include "PacketLifetime.h"

void mt_radio_timing_m97_assertions() {
  // Rejection sentinels, outside any valid driver/host range.
  constexpr uint32_t kDriverSentinel = 0xDEADBEEFu;
  constexpr uint32_t kHostSentinel   = 0xCAFEBABEu;

  // --- REJECT: airMs == 0 (out left unchanged) ---
  {
    mt::RadioTiming::Deadline d{kDriverSentinel, kHostSentinel};
    assert(!mt::RadioTiming::resolve(0u, d));
    assert(d.driverMs == kDriverSentinel);
    assert(d.hostMs == kHostSentinel);
  }

  // --- ACCEPT: airMs=1 -> floor 10000, host 11000 ---
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(1u, d));
    assert(d.driverMs == 10000u);
    assert(d.hostMs == 11000u);
  }

  // --- ACCEPT: airMs=2500 -> floor 10000, host 11000 ---
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(2500u, d));
    assert(d.driverMs == 10000u);
    assert(d.hostMs == 11000u);
  }

  // --- ACCEPT: airMs=4000 -> floor 10000, host 11000 ---
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(4000u, d));
    assert(d.driverMs == 10000u);
    assert(d.hostMs == 11000u);
  }

  // --- ACCEPT: airMs=7600 -> tmo exactly at floor 10000, host 11000 ---
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(7600u, d));
    assert(d.driverMs == 10000u);
    assert(d.hostMs == 11000u);
  }

  // --- ACCEPT: airMs=7601 -> tmo 10001 (just above floor), host 11001 ---
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(7601u, d));
    assert(d.driverMs == 10001u);
    assert(d.hostMs == 11001u);
  }

  // --- ACCEPT: airMs=229000 -> unclamped tmo 286750, capped 262143 ---
  //     airMs < clamped (229000 < 262143) => ACCEPTED; host 263143.
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(229000u, d));
    assert(d.driverMs == 262143u);
    assert(d.hostMs == 263143u);
  }

  // --- ACCEPT: airMs=262142 -> unclamped tmo 328177, capped 262143 ---
  //     airMs < clamped (262142 < 262143) => ACCEPTED; host 263143.
  {
    mt::RadioTiming::Deadline d{0u, 0u};
    assert(mt::RadioTiming::resolve(262142u, d));
    assert(d.driverMs == 262143u);
    assert(d.hostMs == 263143u);
  }

  // --- REJECT: airMs=262143 -> clamped 262143, airMs >= clamped (out unchanged) ---
  {
    mt::RadioTiming::Deadline d{kDriverSentinel, kHostSentinel};
    assert(!mt::RadioTiming::resolve(262143u, d));
    assert(d.driverMs == kDriverSentinel);
    assert(d.hostMs == kHostSentinel);
  }

  // --- REJECT: airMs=UINT32_MAX -> clamped 262143, airMs >= clamped (out unchanged) ---
  {
    mt::RadioTiming::Deadline d{kDriverSentinel, kHostSentinel};
    assert(!mt::RadioTiming::resolve(UINT32_MAX, d));
    assert(d.driverMs == kDriverSentinel);
    assert(d.hostMs == kHostSentinel);
  }
}

void m97_tx_admission_assertions() {
    const float nan_f = std::numeric_limits<float>::quiet_NaN();
    const float pos_inf_f = std::numeric_limits<float>::infinity();
    const float neg_inf_f = -std::numeric_limits<float>::infinity();

    // channel(): unknown is checked before any numeric validation.
    assert(mt::TxAdmission::channel(false, 10.0f, true) == mt::TxAdmission::Decision::Unavailable);
    assert(mt::TxAdmission::channel(false, 10.0f, false) == mt::TxAdmission::Decision::Unavailable);
    assert(mt::TxAdmission::channel(false, nan_f, true) == mt::TxAdmission::Decision::Unavailable);
    assert(mt::TxAdmission::channel(false, 100.0f, false) == mt::TxAdmission::Decision::Unavailable);

    // channel(): strict thresholds — polite < 25, non-polite < 40.
    assert(mt::TxAdmission::channel(true, 24.999f, true) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::channel(true, 25.0f, true) == mt::TxAdmission::Decision::Block);
    assert(mt::TxAdmission::channel(true, 24.999f, false) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::channel(true, 39.999f, false) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::channel(true, 40.0f, false) == mt::TxAdmission::Decision::Block);
    assert(mt::TxAdmission::channel(true, 30.0f, true) == mt::TxAdmission::Decision::Block);
    assert(mt::TxAdmission::channel(true, 30.0f, false) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::channel(true, 0.0f, true) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::channel(true, 100.0f, false) == mt::TxAdmission::Decision::Block);

    // channel(): non-finite or outside [0,100] -> Invalid.
    assert(mt::TxAdmission::channel(true, nan_f, true) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::channel(true, pos_inf_f, false) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::channel(true, neg_inf_f, false) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::channel(true, -0.001f, true) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::channel(true, 100.001f, false) == mt::TxAdmission::Decision::Invalid);

    // duty(): duty/margin validity is checked first, even under override.
    assert(mt::TxAdmission::duty(false, nan_f, 90.0f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, 20.0f, nan_f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, -0.5f, 90.0f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, 100.5f, 90.0f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, 20.0f, 100.5f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, pos_inf_f, 90.0f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, 20.0f, pos_inf_f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(true, nan_f, 90.0f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(true, 20.0f, nan_f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(true, 100.5f, 90.0f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(true, 20.0f, 100.5f, true, 5.0f) == mt::TxAdmission::Decision::Invalid);

    // duty(): override / effectiveDuty==100 bypass only after duty/margin
    // validation; known flag and txPercent are irrelevant after bypass.
    assert(mt::TxAdmission::duty(true, 20.0f, 90.0f, false, nan_f) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::duty(true, 20.0f, 90.0f, false, 150.0f) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::duty(false, 100.0f, 90.0f, false, nan_f) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::duty(false, 100.0f, 0.0f, false, pos_inf_f) == mt::TxAdmission::Decision::Allow);

    // duty(): unknown (valid duty/margin, no bypass) -> Unavailable,
    // checked before txPercent validation.
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, false, 17.999f) == mt::TxAdmission::Decision::Unavailable);
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, false, nan_f) == mt::TxAdmission::Decision::Unavailable);

    // duty(): txPercent validated only when known and no bypass.
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, true, nan_f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, true, 100.001f) == mt::TxAdmission::Decision::Invalid);
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, true, -0.001f) == mt::TxAdmission::Decision::Invalid);

    // duty(): strict boundary effective=20, margin=90 -> threshold 18.0
    // (20*90/100 = 18.0 exactly).
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, true, 17.999f) == mt::TxAdmission::Decision::Allow);
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, true, 18.0f) == mt::TxAdmission::Decision::Block);
    assert(mt::TxAdmission::duty(false, 20.0f, 90.0f, true, 100.0f) == mt::TxAdmission::Decision::Block);
}

static void testPacketLifetimeResolve()
{
    // Exact success vectors (fieldwise only).
    {
        mt::PacketLifetime::Bounds b{};
        assert(mt::PacketLifetime::resolve(1u, 1u, b));
        assert(b.queueMs == 30000u);
        assert(b.pendingMs == 90000u);
        assert(b.historyMs == 600000u);
    }
    {
        mt::PacketLifetime::Bounds b{};
        assert(mt::PacketLifetime::resolve(10000u, 15000u, b));
        assert(b.queueMs == 30000u);
        assert(b.pendingMs == 90000u);
        assert(b.historyMs == 600000u);
    }
    {
        mt::PacketLifetime::Bounds b{};
        assert(mt::PacketLifetime::resolve(263143u, 500000u, b));
        assert(b.queueMs == 273143u);
        assert(b.pendingMs == 2292429u);
        assert(b.historyMs == 2566572u);
    }
    {
        mt::PacketLifetime::Bounds b{};
        assert(mt::PacketLifetime::resolve(263143u, 18800000u, b));
        assert(b.queueMs == 273143u);
        assert(b.pendingMs == 57192429u);
        assert(b.historyMs == 57466572u);
    }

    // Zero host: reject; each output field preserved (distinct sentinels).
    {
        mt::PacketLifetime::Bounds b{};
        b.queueMs = 0x11111111u; b.pendingMs = 0x22222222u; b.historyMs = 0x33333333u;
        assert(!mt::PacketLifetime::resolve(0u, 1u, b));
        assert(b.queueMs == 0x11111111u);
        assert(b.pendingMs == 0x22222222u);
        assert(b.historyMs == 0x33333333u);
    }

    // Zero RTO: reject; each output field preserved.
    {
        mt::PacketLifetime::Bounds b{};
        b.queueMs = 0x44444444u; b.pendingMs = 0x55555555u; b.historyMs = 0x66666666u;
        assert(!mt::PacketLifetime::resolve(1u, 0u, b));
        assert(b.queueMs == 0x44444444u);
        assert(b.pendingMs == 0x55555555u);
        assert(b.historyMs == 0x66666666u);
    }

    // UINT32_MAX inputs: reject; each output field preserved.
    {
        mt::PacketLifetime::Bounds b{};
        b.queueMs = 0x77777777u; b.pendingMs = 0x88888888u; b.historyMs = 0x99999999u;
        assert(!mt::PacketLifetime::resolve(UINT32_MAX, UINT32_MAX, b));
        assert(b.queueMs == 0x77777777u);
        assert(b.pendingMs == 0x88888888u);
        assert(b.historyMs == 0x99999999u);
    }

    // Exact half-wrap boundary: pending is below the boundary, queue is
    // 31000, and history computes to exactly 0x80000000; reject atomically.
    {
        mt::PacketLifetime::Bounds b{};
        b.queueMs = 0xAAAABBBBu; b.pendingMs = 0xCCCDDEEFu; b.historyMs = 0xF0F0F0F0u;
        assert(!mt::PacketLifetime::resolve(21000u, 715795216u, b));
        assert(b.queueMs == 0xAAAABBBBu);
        assert(b.pendingMs == 0xCCCDDEEFu);
        assert(b.historyMs == 0xF0F0F0F0u);
    }
}

int main(){mt_radio_timing_m97_assertions();std::puts("PASS driver-derived finite timeout and atomic refusal boundaries");m97_tx_admission_assertions();std::puts("PASS measured channel strict thresholds and regional duty bypass boundaries");testPacketLifetimeResolve();std::puts("PASS immutable lifetime policy vectors and half-wrap overflow refusal");}
