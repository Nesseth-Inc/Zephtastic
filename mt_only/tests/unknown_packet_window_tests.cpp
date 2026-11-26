// SPDX-License-Identifier: GPL-3.0-only
#include "modules/UnknownPacketWindow.h"
#include <assert.h>
using mt::traffic::UnknownPacketWindow;
int main() {
  // 1: threshold zero leaves state byte fields untouched.
  UnknownPacketWindow::State s{7, 4};
  assert(!UnknownPacketWindow::drop(s, 900000, 0) && s.count == 7 && s.tick == 4);
  // 2: threshold one permits the first packet and drops the second in the same minute.
  s = {};
  assert(!UnknownPacketWindow::drop(s, 0, 1) && s.count == 1 && s.tick == 0);
  assert(UnknownPacketWindow::drop(s, 59999, 1) && s.count == 2);
  // 3: tick advances only when elapsed modular nibble reaches five; a packet does not slide it.
  s = {};
  assert(!UnknownPacketWindow::drop(s, 0, 2));
  assert(!UnknownPacketWindow::drop(s, 4U * 60000U, 2) && s.count == 2 && s.tick == 0);
  assert(!UnknownPacketWindow::drop(s, 5U * 60000U, 2) && s.count == 1 && s.tick == 5);
  // 4: nibble wrap 15->3 is four ticks; 15->4 is five and resets.
  s = {};
  assert(!UnknownPacketWindow::drop(s, 15U * 60000U, 2) && s.tick == 15);
  assert(!UnknownPacketWindow::drop(s, 19U * 60000U, 2) && s.count == 2 && s.tick == 15);
  assert(!UnknownPacketWindow::drop(s, 20U * 60000U, 2) && s.count == 1 && s.tick == 4);
  // 5: threshold is capped at 60; 60 pass, 61 drops, count saturates at 63.
  s = {};
  for (unsigned i = 0; i < 60; ++i) assert(!UnknownPacketWindow::drop(s, 0, UINT32_MAX));
  assert(s.count == 60 && UnknownPacketWindow::drop(s, 0, UINT32_MAX) && s.count == 61);
  for (unsigned i = 0; i < 5; ++i) assert(UnknownPacketWindow::drop(s, 0, UINT32_MAX));
  assert(s.count == 63);
  // 6: reset clears count and tick and permits a fresh first packet.
  UnknownPacketWindow::reset(s);
  assert(s.count == 0 && s.tick == 0 && !UnknownPacketWindow::drop(s, 7U * 60000U, 1));
  assert(s.count == 1 && s.tick == 7);
  // 7: count saturation never wraps after repeated drops.
  for (unsigned i = 0; i < 80; ++i) (void)UnknownPacketWindow::drop(s, 7U * 60000U, 1);
  assert(s.count == 63);
  // 8: call at the exact five-tick boundary starts a new window (first packet allowed).
  UnknownPacketWindow::reset(s);
  assert(!UnknownPacketWindow::drop(s, 10U * 60000U, 1));
  assert(!UnknownPacketWindow::drop(s, 15U * 60000U, 1) && s.count == 1 && s.tick == 15);
}
