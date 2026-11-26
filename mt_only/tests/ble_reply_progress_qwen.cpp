// SPDX-License-Identifier: GPL-3.0-only
// ble_reply_progress_fixture.cpp
// Contract checks for mt::ble::BleReplyProgress using the public API only.
// Returns 0 only if every check passes; nonzero on the first failed check.
#include "BleReplyProgress.h"
#include <cstdio>
#include <stdint.h>

namespace {
int g_failed = 0;
char g_label[48];

void mark_fail(const char* label) {
  if (g_failed == 0) {
    g_failed = 1;
    std::snprintf(g_label, sizeof(g_label), "%s", label);
  }
}

#define CHECK(cond, label) do { if (!(cond)) mark_fail(label); } while (0)
}  // namespace

int main() {
  using mt::ble::BleReplyProgress;
  using State=mt::ble::BleReplyProgress::State;
  BleReplyProgress p;

  CHECK(p.state() == State::Idle && p.covered() == 0, "default-idle");
  CHECK(p.poll(1000) == State::Idle && p.state() == State::Idle, "poll-idle");

  CHECK(!p.begin(0, 8, 100, 10) && !p.begin(7, 0, 100, 10) && !p.begin(7, 513, 100, 10), "invalid-begin-a");
  CHECK(!p.begin(7, 8, 100, 0) && !p.begin(7, 8, 100, 10001) && !p.begin(7, 8, UINT64_MAX, 1), "invalid-begin-b");
  CHECK(p.state() == State::Idle && p.covered() == 0, "invalid-begins-left-state");

  CHECK(p.begin(7, 8, 100, 10) && p.state() == State::Waiting && p.covered() == 0, "valid-begin");
  CHECK(!p.begin(9, 4, 100, 10) && p.state() == State::Waiting && p.covered() == 0, "second-begin");
  CHECK(!p.read(9, 0, 3, 115) && p.state() == State::Waiting && p.covered() == 0, "stale-read");
  CHECK(!p.read(0, 0, 3, 105) && p.state() == State::Waiting && p.covered() == 0, "zero-session-read");

  CHECK(!p.read(7, 0, 0, 105) && !p.read(7, 3, 2, 105) && !p.read(7, 0, 9, 105), "invalid-reads");
  CHECK(p.covered() == 0 && p.state() == State::Waiting, "invalid-read-progress");

  CHECK(p.read(7, 0, 3, 105) && p.covered() == 3, "prefix-read");
  CHECK(p.read(7, 1, 4, 106) && p.covered() == 5, "overlap-read");
  CHECK(p.read(7, 0, 3, 106) && p.covered() == 5 && p.state() == State::Waiting, "duplicate-read");

  CHECK(p.read(7, 5, 3, 109) && p.covered() == 8 && p.state() == State::Drained, "drain-read");
  CHECK(!p.begin(7, 8, 100, 10) && !p.read(7, 0, 1, 109), "drained-busy");
  CHECK(p.poll(109) == State::Drained && p.state() == State::Drained, "drained-poll");
  p.cancel(7);
  CHECK(p.state() == State::Drained, "drained-cancel");

  p.reset();
  CHECK(p.state() == State::Idle && p.covered() == 0, "reset-idle");

  CHECK(p.begin(7, 8, 100, 10) && p.poll(110) == State::Expired, "deadline-expiry");
  CHECK(!p.begin(7, 8, 100, 10), "expired-busy");

  p.reset();
  CHECK(p.begin(7, 8, 100, 10) && !p.read(7, 0, 1, 99) && p.state() == State::Expired, "backclock-expiry");

  p.reset();
  CHECK(p.begin(7, 8, 100, 10), "cancel-begin");
  p.cancel(9);
  CHECK(p.state() == State::Waiting, "wrong-cancel");
  p.cancel(7);
  CHECK(p.state() == State::Cancelled && !p.begin(7, 8, 100, 10), "cancel-terminal");

  p.reset();
  CHECK(p.state() == State::Idle, "final-reset");

  if (g_failed) {
    std::printf("ble_reply_progress_fixture: FAIL %s\n", g_label);
    return 1;
  }
  std::printf("ble_reply_progress_fixture: PASS\n");
  return 0;
}