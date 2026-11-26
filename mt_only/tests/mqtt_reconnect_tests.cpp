#include "MqttReconnectScheduler.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

using mt::mqtt::MqttReconnectScheduler;
using A = MqttReconnectScheduler::Action;
using I = MqttReconnectScheduler::Inputs;
using L = MqttReconnectScheduler::Link;

static I ready(uint32_t now) {
  I in{};
  in.nowMs = now;
  in.mqttEnabled = true;
  in.wantsLink = true;
  in.networkReady = true;
  in.link = L::Wifi;
  in.stockWifiFailureCounter = true;
  in.wifiReconnectAvailable = true;
  return in;
}

static void initialFailureRetryAndSuccessReset() {
  MqttReconnectScheduler s;
  auto in = ready(100);
  assert(s.next(in).action == A::AttemptBrokerConnect);
  assert(s.next(in).action == A::WaitForConnectResult);
  assert(s.connectFailed(100));
  assert(s.failures() == 1);
  assert(s.next(in).action == A::WaitForBrokerBackoff);
  auto backoff = s.next(in);
  assert(backoff.waitMs == 30000);
  in.nowMs = 30099;
  assert(s.next(in).waitMs == 1);
  in.nowMs = 30100;
  assert(s.next(in).action == A::AttemptBrokerConnect);
  assert(s.connectSucceeded());
  assert(s.failures() == 0);
  in = ready(30101);
  in.brokerConnected = true;
  auto online = s.next(in);
  assert(online.action == A::PollConnectedBroker && online.waitMs == 20);
  assert(online.consecutiveFailures == 0);
}

static void fiveFailureCases() {
  MqttReconnectScheduler wifi;
  uint32_t now = 1000;
  I in = ready(now);
  for (int i = 0; i < 5; ++i) {
    in.nowMs = now;
    assert(wifi.next(in).action == A::AttemptBrokerConnect);
    assert(wifi.connectFailed(now));
    if (i < 4) {
      now += 30000;
      in.nowMs = now;
    }
  }
  in.nowMs = now;
  auto request = wifi.next(in);
  assert(request.action == A::RequestWifiReconnect);
  assert(request.consecutiveFailures == 0);
  assert(request.waitMs == 30000);
  assert(wifi.next(in).action == A::WaitForBrokerBackoff);

  MqttReconnectScheduler noHelper;
  now = 2000;
  in = ready(now); in.wifiReconnectAvailable = false;
  for (int i = 0; i < 5; ++i) {
    in.nowMs = now;
    assert(noHelper.next(in).action == A::AttemptBrokerConnect);
    assert(noHelper.connectFailed(now));
    if (i < 4) { now += 30000; in.nowMs = now; }
  }
  auto noRequest = noHelper.next(in);
  assert(noRequest.action == A::WaitForBrokerBackoff);
  assert(noRequest.consecutiveFailures == 0);

  MqttReconnectScheduler ethernet;
  now = 3000;
  in = ready(now); in.link = L::Ethernet;
  for (int i = 0; i < 5; ++i) {
    in.nowMs = now;
    assert(ethernet.next(in).action == A::AttemptBrokerConnect);
    assert(ethernet.connectFailed(now));
    if (i < 4) { now += 30000; in.nowMs = now; }
  }
  auto wait = ethernet.next(in);
  assert(wait.action == A::WaitForBrokerBackoff);
  assert(wait.consecutiveFailures == 0);

  // Builds without stock's HAS_WIFI && !ARCH_PORTDUINO counter must never
  // synthesize its five-failure reconnect threshold.
  MqttReconnectScheduler noCounter;
  now = 4000;
  in = ready(now); in.stockWifiFailureCounter = false;
  for (int i = 0; i < 5; ++i) {
    in.nowMs = now;
    assert(noCounter.next(in).action == A::AttemptBrokerConnect);
    assert(noCounter.connectFailed(now));
    now += 30000;
  }
  in.nowMs = now;
  assert(noCounter.next(in).action == A::AttemptBrokerConnect);
  assert(noCounter.failures() == 0);
}

static void networkAndTlsGates() {
  MqttReconnectScheduler s;
  auto in = ready(500);
  in.networkReady = false;
  assert(s.next(in).action == A::WaitForNetwork);
  assert(s.failures() == 0 && !s.attemptInFlight());
  in.networkReady = true;
  s.networkChanged();
  assert(s.next(in).action == A::AttemptBrokerConnect);
  assert(s.connectAborted());
  assert(!s.attemptInFlight() && s.failures() == 0);
  in.networkReady = false;
  assert(s.next(in).action == A::WaitForNetwork);
  in.networkReady = true;
  s.networkChanged();
  assert(s.next(in).action == A::AttemptBrokerConnect);
  assert(s.connectFailed(500));

  s.reset(); in = ready(9000); in.tlsEnabled = true;
  in.tlsClockReady = false; in.tlsTrustReady = true;
  assert(s.next(in).action == A::WaitForTlsClock);
  assert(s.failures() == 0 && !s.attemptInFlight());
  in.tlsClockReady = true; in.tlsTrustReady = false;
  assert(s.next(in).action == A::WaitForTlsTrust);
  assert(s.failures() == 0 && !s.attemptInFlight());
  in.tlsTrustReady = true;
  assert(s.next(in).action == A::AttemptBrokerConnect);
}

static void stockGatesAndTimerWrap() {
  MqttReconnectScheduler s;
  I in{}; in.nowMs = 0; in.mqttEnabled = false;
  assert(s.next(in).action == A::Disabled);
  in.mqttEnabled = true;
  assert(s.next(in).action == A::NotWanted);
  assert(s.next(in).waitMs == 5000);
  in.brokerConnected = true;
  assert(s.next(in).action == A::DisconnectUnwantedBroker);
  assert(s.next(in).waitMs == 20);
  in.brokerConnected = false;
  in.wantsLink = true; in.proxyToClient = true;
  assert(s.next(in).action == A::ProxyOwnedElsewhere);
  assert(s.next(in).waitMs == 200);

  s.reset(); in = ready(UINT32_MAX - 10000);
  assert(s.next(in).action == A::AttemptBrokerConnect);
  assert(s.connectFailed(in.nowMs));
  in.nowMs += 29999;
  assert(s.next(in).waitMs == 1);
  ++in.nowMs;
  assert(s.next(in).action == A::AttemptBrokerConnect);
  assert(s.connectFailed(in.nowMs));
  in.brokerConnected = true;
  auto online = s.next(in);
  assert(online.action == A::PollConnectedBroker && online.consecutiveFailures == 0);
}

int main() {
  initialFailureRetryAndSuccessReset();
  fiveFailureCases();
  networkAndTlsGates();
  stockGatesAndTimerWrap();
  puts("PASS MQTT reconnect scheduler fake-clock cases");
}
