// SPDX-License-Identifier: GPL-3.0-only
#include "PendingBluetoothAdmin.h"

#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond) \
  do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
} // namespace

namespace mt {
namespace ble {

int runFixture() {
  PendingBluetoothAdmin admin;
  auto req = [](uint32_t s, uint32_t p, bool ack, bool resp, BluetoothSettings d) {
    PendingBluetoothAdmin::Request r;
    r.session = s;
    r.packetId = p;
    r.wantAck = ack;
    r.wantResponse = resp;
    r.desired = d;
    return r;
  };
  auto cfg = [](uint8_t mode, uint32_t pin) {
    BluetoothSettings s;
    s.mode = mode;
    s.fixedPin = pin;
    return s;
  };

  CHECK(admin.state() == PendingBluetoothAdmin::State::Empty && !admin.busy());

  const auto staged = req(7, 0, true, true, cfg(BluetoothSettings::FixedPin, 0)); // packetId 0 and FixedPin 0 legal
  CHECK(admin.stage(staged, true) && admin.state() == PendingBluetoothAdmin::State::Pending && admin.busy());
  CHECK(!admin.stage(req(8, 1, false, false, cfg(BluetoothSettings::RandomPin, 0)), true)); // one pending slot
  CHECK(!admin.stage(req(0, 1, false, false, cfg(BluetoothSettings::FixedPin, 0)), true)); // session zero rejected
  BluetoothSettings badMode; badMode.mode = 3; // mode outside RandomPin/FixedPin/NoPin
  CHECK(!BluetoothSettings::valid(badMode, true) && !admin.stage(req(9, 1, false, false, badMode), true));
  const auto headless = cfg(BluetoothSettings::RandomPin, 0);
  CHECK(!BluetoothSettings::valid(headless, false) && !admin.stage(req(10, 1, false, false, headless), false));
  CHECK(admin.state() == PendingBluetoothAdmin::State::Pending);

  PendingBluetoothAdmin::Request claimed;
  CHECK(admin.claim(7, claimed) && admin.state() == PendingBluetoothAdmin::State::Committing);
  CHECK(claimed.session == staged.session && claimed.packetId == staged.packetId && claimed.wantAck == staged.wantAck && claimed.wantResponse == staged.wantResponse);
  CHECK(claimed.desired.same(staged.desired));

  admin.disconnected(7);
  PendingBluetoothAdmin::Completion out;
  CHECK(admin.finish(7, 0, out) && out.session == 7 && out.packetId == 0 && out.wantAck && out.wantResponse);
  CHECK(!out.replyAllowed && out.result == 0 && admin.state() == PendingBluetoothAdmin::State::Empty && !admin.busy());

  CHECK(admin.stage(req(11, 5, false, false, cfg(BluetoothSettings::NoPin, 0)), true));
  PendingBluetoothAdmin::Request cleared;
  PendingBluetoothAdmin::Request staleOut;
  CHECK(!admin.claim(99, staleOut) && admin.state() == PendingBluetoothAdmin::State::Empty && !admin.busy());
  CHECK(staleOut.session == cleared.session && staleOut.packetId == cleared.packetId && !staleOut.wantAck && !staleOut.wantResponse && staleOut.desired.same(cleared.desired));

  CHECK(admin.stage(req(13, 1, false, false, cfg(BluetoothSettings::FixedPin, 42)), true));
  admin.disconnected(999);
  CHECK(admin.state() == PendingBluetoothAdmin::State::Pending);
  admin.disconnected(13); CHECK(admin.state() == PendingBluetoothAdmin::State::Empty);

  CHECK(admin.stage(req(17, 2, true, false, cfg(BluetoothSettings::RandomPin, 0)), true));
  CHECK(admin.claim(17, claimed) && admin.state() == PendingBluetoothAdmin::State::Committing);
  CHECK(admin.finish(17, 1, out) && out.session == 17 && out.packetId == 2 && out.wantAck && !out.wantResponse);
  CHECK(out.replyAllowed && out.result == 1 && admin.state() == PendingBluetoothAdmin::State::Empty);

  CHECK(admin.stage(req(21, 3, false, false, cfg(BluetoothSettings::NoPin, 0)), true));
  CHECK(!admin.finish(21, 0, out) && out.session == 0 && out.packetId == 0 && out.result == 0 && !out.wantAck && !out.wantResponse && !out.replyAllowed);
  admin.disconnected(21);
  CHECK(admin.state() == PendingBluetoothAdmin::State::Empty);
  CHECK(!admin.finish(99, 0, out) && out.session == 0 && out.packetId == 0 && out.result == 0 && !out.wantAck && !out.wantResponse && !out.replyAllowed);

  return failures;
}

} // namespace ble
} // namespace mt

int main() {
  return mt::ble::runFixture();
}
