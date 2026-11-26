// SPDX-License-Identifier: GPL-3.0-only
// M60 storage-pause radio-trace parity test (m60-pause-trace-tests-a1).
//
// The parent compiles this file twice: against the original immutable
// Service.h (without STORAGE_DIAGNOSTICS_TEST) and against the instrumented
// Service.h (with -DSTORAGE_DIAGNOSTICS_TEST=1). The canonical stdout lines
// (case name, trace, result, receiving, fault) are byte-identical in both
// builds. The storage-diagnostics snapshot checks are gated on
// STORAGE_DIAGNOSTICS_TEST and use assert() only, so they add no stdout and
// cannot alter the canonical output. Expected values are the contract
// literals from the task, not re-derived implementation logic.
//
// Canonical line format (identical in both builds):
//   <name> trace=<R/B/H/N...> result=<int> receiving=<0|1> fault=<0|1>
#include "Service.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Minimal fake radio: records the exact native call sequence in `trace`
// (ready=R, busy=B, health=H, stopForStorage=N) with no real I/O.
struct Fake : mt::Radio {
  std::string trace;
  int healthVal = 0;
  bool readyVal = true;
  int stopRet = 0;
  int postHealth = 0;
  std::vector<bool> busySeq;
  size_t busyIdx = 0;
  bool busyDefault = false;
  int sends = 0;
  bool ready() override { trace += "R"; return readyVal; }
  int start() override { healthVal = 1; return 0; }
  int restore() override { return 0; }
  int stop() override { return 0; }
  int stopForStorage() override {
    trace += "N";
    if (stopRet == 0) healthVal = postHealth;
    return stopRet;
  }
  int health() override { trace += "H"; return healthVal; }
  bool busy() override {
    trace += "B";
    if (busyIdx < busySeq.size()) return busySeq[busyIdx++];
    return busyDefault;
  }
  uint32_t airtime(size_t) override { return 0; }
  int send(uint8_t *, size_t) override { ++sends; return 0; }
  bool completion(int &r) override { r = 0; return false; }
};

struct Case {
  const char *name;
  std::string wantTrace;
  int wantResult;
  bool wantReceiving;
  bool wantFault;
  bool rx;
  bool ready;
  int health;
  int stopRet;
  int postHealth;
  std::vector<bool> busy;
  uint8_t wantReason;
};

// Prints the canonical line (identical in both builds) and asserts the
// contract values. Diagnostics are asserted separately, under the gate.
void check(const Case &c, const std::string &trace, int result,
           bool receiving, bool fault) {
  std::printf("%s trace=%s result=%d receiving=%d fault=%d\n", c.name,
              trace.c_str(), result, (int)receiving, (int)fault);
  assert(trace == c.wantTrace);
  assert(result == c.wantResult);
  assert(receiving == c.wantReceiving);
  assert(fault == c.wantFault);
}

} // namespace

int main() {
  const Case cases[] = {
    // name,                wantTrace, wantResult, wantRx, wantFault, rx, ready, health, stopRet, postHealth, busy,         reason
    {"c1-off-healthy-H0", "BRBH", 0, false, false, false, true, 0, 0, 0, {}, 0},
    {"c2-off-not-ready", "BR", -EIO, false, false, false, false, 0, 0, 0, {}, 3},
    {"c3-off-busy-2nd", "BRB", -EIO, false, false, false, true, 0, 0, 0, {false, true}, 3},
    {"c4-off-H1", "BRBH", -EIO, false, false, false, true, 1, 0, 0, {}, 3},
    {"c5-rx-busy-first", "B", -EBUSY, true, false, true, true, 1, 0, 0, {true}, 2},
    {"c6-rx-eagain", "BN", -EBUSY, true, false, true, true, 1, -EAGAIN, 0, {}, 4},
    {"c7-rx-eio", "BN", -EIO, true, true, true, true, 1, -EIO, 0, {}, 5},
    {"c8-rx-eshutdown", "BN", -ESHUTDOWN, false, true, true, true, 1, -ESHUTDOWN, 0, {}, 5},
    {"c9-rx-posthealth1", "BNH", -EIO, true, true, true, true, 1, 0, 1, {}, 6},
    {"c10-rx-posthealth0", "BNH", 0, false, false, true, true, 1, 0, 0, {}, 0},
  };
  for (const Case &c : cases) {
    Fake fake;
    fake.readyVal = c.ready;
    fake.healthVal = c.health;
    fake.stopRet = c.stopRet;
    fake.postHealth = c.postHealth;
    fake.busySeq = c.busy;
    mt::Service svc(fake, true, 2, 81);
    mt::Config cfg;
    cfg.bytes[4] = 1;
    cfg.seal();
    assert(svc.apply(cfg));
    if (c.rx) {
      assert(svc.start(false, 0));
      fake.trace.clear();
    }
    const int ret = svc.pauseForStorage();
    check(c, fake.trace, ret, svc.receiving(), svc.faulted());
    assert(fake.sends == 0);
#ifdef STORAGE_DIAGNOSTICS_TEST
    const mt::StoragePauseDiagnostic d = svc.storagePauseDiagnostic();
    assert(d.result == ret);
    assert(int(d.reason) == c.wantReason);
    assert(d.mask == 0);
    assert(d.sequence == 1);
#endif
  }
  // Case 7 leaves a faulted, still-receiving service. A second pause on that
  // same service must be rejected with no radio traffic.
  {
    Fake fake;
    fake.stopRet = -EIO;
    mt::Service svc(fake, true, 2, 81);
    mt::Config cfg;
    cfg.bytes[4] = 1;
    cfg.seal();
    assert(svc.apply(cfg));
    assert(svc.start(false, 0));
    fake.trace.clear();
    assert(svc.pauseForStorage() == -EIO); // first pause faults the service
    fake.trace.clear();
    const int ret = svc.pauseForStorage(); // second pause on the faulted service
    const Case c7b{"c7b-second-faulted", "", -EBUSY, true, true, false, true, 1, -EIO, 0, {}, 1};
    check(c7b, fake.trace, ret, svc.receiving(), svc.faulted());
    assert(fake.sends == 0);
#ifdef STORAGE_DIAGNOSTICS_TEST
    const mt::StoragePauseDiagnostic d = svc.storagePauseDiagnostic();
    assert(d.result == ret);
    assert(int(d.reason) == 1);
    assert(d.mask == 2);
    assert(d.sequence == 2);
#endif
  }
  return 0;
}
