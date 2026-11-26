// SPDX-License-Identifier: GPL-3.0-only
// stock_per_source_rate_window_tests.cpp
// Behavior tests for mt::traffic::StockPerSourceRateWindow (locked API).
// Pinned contract (Meshtastic TrafficManagementModule::isRateLimited):
// nonzero seconds quantize to 5-minute ticks clamped 1..15; zero window
// or zero cap disables without mutating state; cap capped at 60; count
// starts at 1 and saturates at 63; 4-bit modular elapsed delta >= window
// restarts the window at count 1; first N pass, then deny.

#include "modules/StockPerSourceRateWindow.h"
#include <cassert>

using mt::traffic::StockPerSourceRateWindow;

int main() {
  // (1) Disabled calls (zero window or zero cap) are no-ops: they neither
  // consume slots nor initialize the window.
  {
    StockPerSourceRateWindow w;
    assert(w.allow(0, 300, 2));        // first packet: count 1
    assert(w.allow(0, 0, 2));          // zero window: disabled
    assert(w.allow(0, 300, 0));        // zero cap: disabled
    for (int i = 0; i < 8; ++i)
      assert(w.allow(0, 0, 2));        // many disabled calls: no state change
    assert(w.allow(0, 300, 2));        // exactly one more allowed: count 2
    assert(!w.allow(0, 300, 2));       // then deny: count 3 > cap 2

    StockPerSourceRateWindow fresh;
    assert(fresh.allow(0, 0, 2));      // disabled before init: no state set
    assert(fresh.allow(0, 300, 1));    // first real call: count 1
    assert(!fresh.allow(0, 300, 1));   // second: count 2 > cap 1
  }

  // (2) 300s = 1 tick: same tick stays, next tick restarts; 4-bit modular
  // wrap 15 -> 0 is one tick later. 600s = 2 ticks: +1 inside, +2 restarts.
  {
    StockPerSourceRateWindow w;
    assert(w.allow(0, 300, 5));        // start at tick 0
    assert(w.allow(0, 300, 5));        // same tick: still inside
    assert(w.allow(1, 300, 5));        // elapsed 1 >= 1: restart
    assert(w.allow(15, 300, 5));       // restart at tick 15
    assert(w.allow(0, 300, 5));        // (0 - 15) & 0xF = 15 >= 1: restart

    StockPerSourceRateWindow two;
    assert(two.allow(0, 600, 1));      // start at tick 0, window 2 ticks
    assert(!two.allow(1, 600, 1));     // elapsed 1 < 2: count 2 > cap 1
    assert(two.allow(2, 600, 1));      // elapsed 2 >= 2: restart
  }

  // (3) Configured cap above 60 is capped at 60; repeated denials must not
  // wrap back into acceptance (count saturates at 63, an implementation
  // contract observed only through sustained denial).
  {
    StockPerSourceRateWindow w;
    for (int i = 0; i < 60; ++i)
      assert(w.allow(0, 300, 100));    // effective cap 60: all pass
    for (int i = 0; i < 300; ++i)
      assert(!w.allow(0, 300, 100));   // sustained denial, no wrap to allow
  }

  // (4) reset() restores the fresh state: the window restarts at count 1.
  {
    StockPerSourceRateWindow w;
    assert(w.allow(0, 300, 2));
    assert(w.allow(0, 300, 2));        // count 2
    assert(!w.allow(0, 300, 2));       // count 3 > 2
    w.reset();
    assert(w.allow(0, 300, 2));        // fresh window: count 1
    assert(w.allow(0, 300, 2));        // count 2
    assert(!w.allow(0, 300, 2));       // count 3 > 2
  }

  return 0;
}
