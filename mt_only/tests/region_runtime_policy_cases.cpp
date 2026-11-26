// SPDX-License-Identifier: GPL-3.0-only
// Region-specific duty and ModuleController interval policy; fake radio only.
#include "ModuleController.h"
#include "RegionRuntimePolicy.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>

#include <iostream>

static void group(const char *name) { std::cout << "PASS " << name << '\n'; }
constexpr uint32_t kNode = 0x12345678u;

static void dutyCases() {
  using D = mt::TxAdmission::Decision;
  using mt::radio::RegionRuntimePolicy::duty;
  for (uint32_t code : {0u, 13u, 15u, 17u, 27u, 30u, 31u, UINT32_MAX})
    assert(duty(code, false, false, true, 0.0f) == D::Unavailable);
  assert(duty(1, false, false, false, NAN) == D::Allow); // US has no duty cap.
  assert(duty(1, true, false, true, 100.0f) == D::Allow);

  // Stock Router::send rejects only above the region's effective duty.
  assert(duty(2, false, false, true, 10.0f) == D::Allow);
  assert(duty(2, false, false, true, 10.001f) == D::Block);
  assert(duty(2, false, false, false, 0.0f) == D::Unavailable);
  assert(duty(2, false, false, true, NAN) == D::Invalid);
  assert(duty(2, false, false, true, -0.01f) == D::Invalid);
  assert(duty(2, false, false, true, 100.01f) == D::Invalid);

  // EU_866 has role-dependent stock limits: 2.5% mobile, 10% fixed router.
  assert(duty(29, false, false, true, 2.5f) == D::Allow);
  assert(duty(29, false, false, true, 2.5001f) == D::Block);
  assert(duty(29, true, false, true, 10.0f) == D::Allow);
  assert(duty(29, true, false, true, 10.001f) == D::Block);

  // Polite scheduling keeps its existing 50% margin and strict-below rule.
  assert(duty(2, false, true, true, 4.999f) == D::Allow);
  assert(duty(2, false, true, true, 5.0f) == D::Block);
  assert(duty(2, false, true, true, 5.001f) == D::Block);
  assert(duty(2, false, true, false, 0.0f) == D::Unavailable);
  assert(duty(2, false, true, true, NAN) == D::Invalid);
  group("regional duty gates fail closed for unsupported bands and match US, mobile/router, equality, unknown, invalid, and polite-margin boundaries");
}

struct TestRadio final : mt::Radio {
  bool ready() override { return true; }
  int start() override { return 0; }
  int restore() override { return 0; }
  int stop() override { return 0; }
  int health() override { return 1; }
  bool busy() override { return false; }
  uint32_t airtime(size_t) override { return 0; }
  int send(uint8_t *, size_t) override { return 0; }
  bool completion(int &) override { return false; }
};

struct Harness {
  TestRadio radio;
  mt::Service service{radio, true, kNode, 127, false, false};

  void configure(uint32_t regionCode, uint32_t preset, unsigned role) {
    auto candidate = service.config;
    auto request = candidate.radioRequest();
    request.regionCode = regionCode;
    request.usePreset = true;
    request.preset = preset;
    request.channel = 0;
    assert(candidate.setRadio(request));
    assert(candidate.setDeviceRole(role, 0));
    assert(service.apply(candidate));
  }
};

static void moduleIntervalCases() {
  // UNSET and unsupported software regions never produce an interval.
  {
    Harness h;
    mt::ModuleController controller;
    uint32_t interval = 999;
    assert(!controller.positionInterval(h.service, 120, 1000, interval));
    assert(interval == 999);
  }
  // US mobile: one observed peer crosses stock's 40-node threshold by one.
  {
    Harness h;
    h.configure(1, 0, 1);
    mt::ModuleController controller;
    uint32_t interval = 0;
    for (uint32_t i = 0; i < 40; ++i)
      controller.observe(100 + i, kNode, 0, 1000, true, -1.0f);
    assert(controller.positionInterval(h.service, 120, 1000, interval));
    assert(interval == 129830u); // 120s * (1 + 1 * 2^11/(250*100)), throttle 1.
  }

  // EU_866 mobile uses LiteFast's 9/125k modulation and position throttle 10.
  {
    Harness h;
    h.configure(29, 10, 1);
    mt::ModuleController controller;
    uint32_t interval = 0;
    for (uint32_t i = 0; i < 40; ++i)
      controller.observe(200 + i, kNode, 0, 1000, true, -1.0f);
    assert(controller.positionInterval(h.service, 120, 1000, interval));
    assert(interval == 1249150u); // scaled 124915ms, then stock *10 profile throttle.
  }

  // Routers use their role default/explicit interval without mesh-size scaling,
  // then apply the selected regional profile multiplier.
  {
    Harness h;
    h.configure(29, 10, 2);
    mt::ModuleController controller;
    uint32_t interval = 0;
    assert(controller.positionInterval(h.service, 120, 1000, interval));
    assert(interval == 1200000u);
    assert(controller.positionInterval(h.service, 0, 1000, interval));
    assert(interval == 432000000u); // 12h fixed-router default, profile throttle 10.
  }

  // Mobile interval scaling is unavailable if the online count clock rolls
  // back. Output remains unchanged; the controller does not guess a value.
  {
    Harness h;
    h.configure(1, 0, 1);
    mt::ModuleController controller;
    uint32_t interval = 77;
    // Seed the monotonic high-water mark by computing before observing peers.
    assert(controller.positionInterval(h.service, 120, 1001, interval));
    interval = 77;
    assert(!controller.positionInterval(h.service, 120, 1000, interval));
    assert(interval == 77);
  }
  group("ModuleController resolves region profile and role: US/EU866 scaling, router defaults, and rollback fail closed");
}

int main() {
  dutyCases();
  moduleIntervalCases();
}
