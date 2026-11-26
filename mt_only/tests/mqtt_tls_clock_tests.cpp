#include "network/MqttTlsClock.h"

#include <assert.h>

using mt::mqtt::CertificateClock;

namespace {
int calls = 0;
int nextResult = 0;
uint32_t lastSeconds = 0;

int fakeSetRealtime(uint32_t seconds) {
  ++calls;
  lastSeconds = seconds;
  return nextResult;
}
}  // namespace

int main() {
  CertificateClock clock;
  constexpr uint32_t goodTime = 1761782400u;
  assert(!clock.ready());

  // Too-old input and missing backend do not call a backend or change state.
  assert(!clock.sync(CertificateClock::kEarliestPlausibleUnixSeconds - 1u, fakeSetRealtime));
  assert(!clock.sync(goodTime, nullptr));
  assert(!clock.ready() && calls == 0);

  // A failed first set keeps readiness false.
  nextResult = -1;
  assert(!clock.sync(goodTime, fakeSetRealtime));
  assert(!clock.ready() && calls == 1 && lastSeconds == goodTime);

  // Success opens the gate; invalid input preserves that established state.
  nextResult = 0;
  assert(clock.sync(goodTime, fakeSetRealtime));
  assert(clock.ready() && calls == 2);
  assert(!clock.sync(0, fakeSetRealtime));
  assert(!clock.sync(goodTime, nullptr));
  assert(clock.ready() && calls == 2);

  // A later actual backend failure closes the gate; a subsequent success restores it.
  nextResult = -5;
  assert(!clock.sync(goodTime + 1u, fakeSetRealtime));
  assert(!clock.ready() && calls == 3 && lastSeconds == goodTime + 1u);
  nextResult = 0;
  assert(clock.sync(goodTime + 2u, fakeSetRealtime));
  assert(clock.ready() && calls == 4 && lastSeconds == goodTime + 2u);
}
