// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

namespace mt { namespace mqtt {

// Deterministic, fixed-storage retry policy. It returns actions only: callers
// own all MQTT sockets, Wi-Fi/ethernet operations, TLS state and clocks.
class MqttReconnectScheduler {
 public:
  static constexpr uint32_t kNoWantCheckMs = 5000;  // stock MQTT::runOnce()
  static constexpr uint32_t kBrokerRetryMs = 30000; // stock failed-connect path
  static constexpr uint32_t kConnectedPollMs = 20;  // stock connected runOnce()
  static constexpr uint8_t kFailuresBeforeWifiRequest = 5; // stock reconnectMax

  enum class Link : uint8_t { None, Wifi, Ethernet, Other };
  enum class Action : uint8_t {
    Disabled,
    NotWanted,
    DisconnectUnwantedBroker,
    ProxyOwnedElsewhere,
    WaitForNetwork,
    WaitForTlsClock,
    WaitForTlsTrust,
    WaitForBrokerBackoff,
    WaitForConnectResult,
    PollConnectedBroker,
    AttemptBrokerConnect,
    RequestWifiReconnect
  };

  struct Inputs {
    uint32_t nowMs = 0;
    bool mqttEnabled = false;
    bool wantsLink = false;  // feature/channel-or-map requirement snapshot
    bool proxyToClient = false;
    bool networkReady = false;
    Link link = Link::None;
    // Models the pinned #if HAS_WIFI && !ARCH_PORTDUINO failure counter.
    bool stockWifiFailureCounter = false;
    bool wifiReconnectAvailable = false;
    bool brokerConnected = false;
    bool tlsEnabled = false;
    bool tlsClockReady = false;
    bool tlsTrustReady = false;
  };

  struct Result {
    Action action = Action::Disabled;
    uint32_t waitMs = 0;
    uint8_t consecutiveFailures = 0;
  };

  // Deterministic state transition from caller-supplied snapshots; no network
  // or platform call occurs here. Returning AttemptBrokerConnect marks one
  // attempt in flight; the caller reports completion through
  // connectSucceeded()/connectFailed().
  Result next(const Inputs &in) {
    if (!in.mqttEnabled) return result(Action::Disabled, 0);
    if (in.brokerConnected && !in.wantsLink && !in.proxyToClient)
      return result(Action::DisconnectUnwantedBroker, kConnectedPollMs);
    if (!in.wantsLink) return result(Action::NotWanted, kNoWantCheckMs);
    if (in.proxyToClient) return result(Action::ProxyOwnedElsewhere, 200);

    if (in.brokerConnected) {
      resetAfterSuccess();
      return result(Action::PollConnectedBroker, kConnectedPollMs);
    }

    // Stock reconnectMax accounting is evaluated immediately after the fifth
    // failed connect. Wi-Fi reconnect is an intent for the caller only.
    if (failures_ >= kFailuresBeforeWifiRequest) {
      failures_ = 0;
      if (in.link == Link::Wifi && in.wifiReconnectAvailable)
        return result(Action::RequestWifiReconnect, remaining(in.nowMs));
    }

    if (!in.networkReady) return result(Action::WaitForNetwork, kNoWantCheckMs);
    if (in.tlsEnabled && !in.tlsClockReady)
      return result(Action::WaitForTlsClock, 0);
    if (in.tlsEnabled && !in.tlsTrustReady)
      return result(Action::WaitForTlsTrust, 0);
    if (inFlight_) return result(Action::WaitForConnectResult, 0);
    if (deadlineArmed_ && !deadlineReached(in.nowMs, retryAtMs_))
      return result(Action::WaitForBrokerBackoff, retryAtMs_ - in.nowMs);

    deadlineArmed_ = false;
    countThisAttempt_ = in.stockWifiFailureCounter;
    inFlight_ = true;
    return result(Action::AttemptBrokerConnect, 0);
  }

  // Report a terminal broker connect failure or connection loss. Failed
  // attempts are bounded at five; the next `next()` yields a Wi-Fi reconnect
  // request when available, otherwise it resets the stock counter and keeps
  // broker backoff. The caller performs that returned intent separately.
  bool connectFailed(uint32_t nowMs) {
    if (!inFlight_) return false;
    inFlight_ = false;
    if (countThisAttempt_ && failures_ < kFailuresBeforeWifiRequest) ++failures_;
    countThisAttempt_ = false;
    retryAtMs_ = nowMs + kBrokerRetryMs;
    deadlineArmed_ = true;
    return true;
  }

  bool connectSucceeded() {
    if (!inFlight_) return false;
    inFlight_ = false;
    failures_ = 0;
    deadlineArmed_ = false;
    retryAtMs_ = 0;
    countThisAttempt_ = false;
    return true;
  }

  // The platform invalidated an in-flight socket attempt (for example Wi-Fi
  // became unavailable). This is not counted as a broker rejection.
  bool connectAborted() {
    if (!inFlight_) return false;
    inFlight_ = false;
    countThisAttempt_ = false;
    return true;
  }

  // An explicit platform network-change event may immediately retry once the
  // caller has fresh readiness/TLS snapshots. No connection is started here.
  void networkChanged() {
    if (!inFlight_) deadlineArmed_ = false;
  }

  void reset() {
    failures_ = 0;
    retryAtMs_ = 0;
    deadlineArmed_ = false;
    inFlight_ = false;
    countThisAttempt_ = false;
  }

  uint8_t failures() const { return failures_; }
  bool attemptInFlight() const { return inFlight_; }

 private:
  Result result(Action action, uint32_t wait) const {
    return Result{action, wait, failures_};
  }
  static bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
  }
  uint32_t remaining(uint32_t now) const {
    return deadlineArmed_ && !deadlineReached(now, retryAtMs_)
               ? retryAtMs_ - now
               : 0;
  }
  void resetAfterSuccess() {
    failures_ = 0;
    inFlight_ = false;
    deadlineArmed_ = false;
    retryAtMs_ = 0;
    countThisAttempt_ = false;
  }

  uint32_t retryAtMs_ = 0;
  uint8_t failures_ = 0;
  bool deadlineArmed_ = false;
  bool inFlight_ = false;
  bool countThisAttempt_ = false;
};

}}  // namespace mt::mqtt
