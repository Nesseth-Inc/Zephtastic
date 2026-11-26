// SPDX-License-Identifier: GPL-3.0-only
// Synthetic public Admin keys only; no identity, device, or RF activity.
#include "admin/AdminKeyCommit.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace mt;
using namespace mt::admin;

struct Backend final : SettingsBackend {
  std::map<std::string, std::vector<uint8_t>> rows;
  unsigned writes = 0;
  int writeError = 0;
  bool corruptReadback = false;

  int read(const char *key, uint8_t *out, size_t capacity) override {
    const auto it = rows.find(key);
    if (it == rows.end()) return -ENOENT;
    if (it->second.size() > capacity) return -E2BIG;
    if (!it->second.empty()) memcpy(out, it->second.data(), it->second.size());
    if (corruptReadback && writes && !it->second.empty()) out[0] ^= 0x40;
    return static_cast<int>(it->second.size());
  }

  int write(const char *key, const uint8_t *bytes, size_t length) override {
    ++writes;
    if (writeError) return writeError;
    rows[key] = std::vector<uint8_t>(bytes, bytes + length);
    return 0;
  }
};

struct TestRadio final : Radio {
  int state = 0, startError = 0, pauseError = 0;
  bool busyNow = false;
  unsigned starts = 0, pauses = 0, sends = 0;
  bool ready() override { return true; }
  int start() override {
    ++starts;
    if (startError) return startError;
    state = 1;
    return 0;
  }
  int stop() override { state = 0; return 0; }
  int restore() override { state = 1; return 0; }
  int stopForStorage() override {
    ++pauses;
    if (pauseError) return pauseError;
    state = 0;
    return 0;
  }
  int health() override { return state; }
  bool busy() override { return busyNow; }
  uint32_t airtime(size_t) override { return 100; }
  int send(uint8_t *, size_t) override { ++sends; return 0; }
  bool completion(int &) override { return false; }
};

static AdminKeySettings key(uint8_t marker) {
  uint8_t packed[AdminKeySettings::KeyBytes];
  memset(packed, marker, sizeof(packed));
  AdminKeySettings result;
  assert(result.assign(1, packed, sizeof(packed)));
  return result;
}

struct Fixture {
  TestRadio radio;
  Backend backend;
  AdminKeyJournal journal;
  test::KnownChannelFixtureClock clock;
  Service service;

  explicit Fixture(bool armed = true)
      : service(radio, true, 0x12345678, 127, true, false) {
    clock.bind(service);
    Config config;
    config.bytes[4] = 1;
    config.setTxEnabled(true);
    auto node = config.node();
    node.setPublic();
    config.node(node);
    assert(service.apply(config) && service.start(false, 0));
    assert(service.armEndpoint(armed));
    assert(journal.load(backend) == 0 && journal.ready());
  }

  int save(const AdminKeySettings &candidate, uint32_t now = 10) {
    return AdminKeyCommit::save(service, radio, journal, backend, candidate, now);
  }
};

static void noOpAndPrePauseRefusalsAreInert() {
  Fixture fixture;
  AdminKeyJournal unloaded;
  assert(AdminKeyCommit::save(fixture.service, fixture.radio, unloaded,
                              fixture.backend, key(0x30), 10) == -EACCES);
  assert(!fixture.backend.writes && !fixture.radio.pauses && fixture.service.receiving());

  assert(fixture.save(AdminKeySettings{}) == 0);
  assert(!fixture.backend.writes && !fixture.radio.pauses && fixture.service.receiving());

  fixture.radio.busyNow = true;
  assert(fixture.save(key(0x31)) == -EBUSY);
  assert(!fixture.backend.writes && !fixture.radio.pauses && fixture.service.receiving());
  fixture.radio.busyNow = false;

}

static void savesRestorePriorReceiveAndArmIntent() {
  for (bool armed : {false, true}) {
    Fixture fixture(armed);
    assert(fixture.save(key(0x42)) == 0);
    assert(fixture.backend.writes == 1 && fixture.journal.settings().same(key(0x42)));
    assert(fixture.backend.rows.size() == 1 && fixture.backend.rows.count("mtadmin/state0"));
    assert(fixture.service.receiving() && fixture.service.endpointArmed() == armed);
    assert(fixture.radio.state == 1 && fixture.radio.pauses == 1 && fixture.radio.sends == 0);
  }
}

static void pauseFailuresNeverWriteOrLoseHealthyReceive() {
  {
    Fixture fixture;
    fixture.radio.pauseError = -EAGAIN;
    assert(fixture.save(key(0x53)) == -EBUSY);
    assert(!fixture.backend.writes && fixture.service.receiving() &&
           fixture.service.endpointArmed() && !fixture.service.faulted());
  }
  {
    Fixture fixture;
    fixture.radio.pauseError = -ESHUTDOWN;
    assert(fixture.save(key(0x54)) == -EIO);
    assert(!fixture.backend.writes && fixture.service.faulted());
  }
}

static void persistenceUncertaintyPoisonsKeysButSettlesService() {
  for (int error : {-EIO, -ENOSPC, -ENOMEM, -E2BIG, -EBUSY}) {
    Fixture fixture;
    fixture.backend.writeError = error;
    assert(fixture.save(key(0x65)) == -EIO);
    assert(fixture.backend.writes == 1 && !fixture.journal.ready());
    assert(fixture.service.receiving() && fixture.service.endpointArmed() &&
           fixture.radio.state == 1 && !fixture.radio.sends);
    assert(fixture.save(key(0x65)) == -EACCES && fixture.backend.writes == 1);
  }

  Fixture readback;
  readback.backend.corruptReadback = true;
  assert(readback.save(key(0x76)) == -EIO && !readback.journal.ready());
  assert(readback.service.receiving() && readback.service.endpointArmed() &&
         readback.radio.state == 1 && !readback.radio.sends);
}

static void restoreFailureIsReportedAndLeavesServiceStopped() {
  Fixture fixture;
  fixture.radio.startError = -EIO;
  assert(fixture.save(key(0x27)) == -EIO);
  assert(fixture.backend.writes == 1 && fixture.journal.ready() &&
         fixture.journal.settings().same(key(0x27)));
  assert(!fixture.service.receiving() && !fixture.service.endpointArmed() &&
         fixture.service.faulted() && fixture.radio.state == 0);
}

int main() {
  noOpAndPrePauseRefusalsAreInert();
  puts("PASS Admin key no-op and pre-pause refusals are inert");
  savesRestorePriorReceiveAndArmIntent();
  puts("PASS Admin key save restores prior RX and arm intent without RF TX");
  pauseFailuresNeverWriteOrLoseHealthyReceive();
  puts("PASS Admin key pause failures do not write or silently discard RX");
  persistenceUncertaintyPoisonsKeysButSettlesService();
  puts("PASS Admin key persistence uncertainty poisons authorization and settles RX");
  restoreFailureIsReportedAndLeavesServiceStopped();
  puts("PASS Admin key restore failure reports degraded stopped service");
}
