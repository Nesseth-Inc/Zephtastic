// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../stock/peripherals/DetectionSensorModule.h"
#include <string.h>

namespace mt::modules::detection {

// Portable stock DetectionSensor policy. It consumes already-sampled logical
// levels and returns bounded effects; it never reads/writes GPIO or radio.
class DetectionSensorPolicy {
public:
  static constexpr uint32_t SampleIntervalMs = 100;
  static constexpr uint32_t DefaultMinimumBroadcastSecs = 45;
  static constexpr size_t NameCapacity = 20;
  static constexpr size_t PayloadCapacity = 40;
  static constexpr uint32_t PortNum = 10;

  enum Trigger : uint32_t {
    LogicLow = 0,
    LogicHigh = 1,
    FallingEdge = 2,
    RisingEdge = 3,
    EitherEdgeActiveLow = 4,
    EitherEdgeActiveHigh = 5
  };

  struct Config {
    bool enabled = false;
    uint32_t monitorPin = 0;
    bool usePullup = false;
    uint32_t minimumBroadcastSecs = 0;
    uint32_t stateBroadcastSecs = 0;
    bool sendBell = false;
    uint32_t trigger = LogicHigh;
    char name[NameCapacity]{};
  };

  enum class ConfigureResult : uint8_t { Accepted, Disabled, UnavailablePin, Invalid };
  enum class PollCode : uint8_t {
    Disabled, Unavailable, InputModeRequested, NotSampleDue, Sampled,
    PublicChannelBlocked, Published, Backpressured
  };
  enum class EventKind : uint8_t { Detection, State };
  enum class Cadence : uint8_t { MinimumInterval, StateHeartbeat };

  struct Event {
    EventKind kind = EventKind::Detection;
    uint32_t port = PortNum;
    bool wantAck = false;
    bool state = false;
    uint8_t payload[PayloadCapacity]{};
    size_t payloadSize = 0;
  };

  struct PollResult {
    PollCode code = PollCode::Disabled;
    bool sampled = false;
    bool requestInputMode = false;
    bool pullup = false;
    bool hasEvent = false;
    Event event{};
    uint32_t coalescedEvents = 0;
  };

private:
  Config config_{};
  bool ready_ = false;
  bool inputModeRequested_ = false;
  bool previousActive_ = false;
  bool haveSample_ = false;
  bool haveAttempt_ = false;
  uint32_t lastSampleMs_ = 0;
  uint32_t lastAttemptMs_ = 0;

  bool pending_ = false;
  EventKind pendingKind_ = EventKind::Detection;
  Cadence pendingCadence_ = Cadence::MinimumInterval;
  bool pendingState_ = false;
  bool deferred_ = false;
  EventKind deferredKind_ = EventKind::Detection;
  Cadence deferredCadence_ = Cadence::MinimumInterval;
  bool deferredState_ = false;
  uint32_t coalescedEvents_ = 0;

  static bool validName(const char *name, size_t &length) {
    if (!name) return false;
    length = 0;
    while (length < NameCapacity && name[length] != '\0') {
      ++length;
    }
    // The stock schema is a bounded protobuf string; preserve UTF-8 bytes and
    // reject only an unterminated fixed-width value.
    return length < NameCapacity;
  }

  static bool validConfig(const Config &config) {
    size_t nameLength = 0;
    if (!validName(config.name, nameLength)) return false;
    if (config.trigger > EitherEdgeActiveHigh) return false;
    if (config.minimumBroadcastSecs > UINT32_MAX / 1000u ||
        config.stateBroadcastSecs > UINT32_MAX / 1000u) return false;
    if (config.enabled && config.monitorPin == 0) return false;
    return true;
  }

  static uint32_t minimumSecs(const Config &config) {
    return config.minimumBroadcastSecs == 0
        ? DefaultMinimumBroadcastSecs : config.minimumBroadcastSecs;
  }
  static uint32_t elapsed(uint32_t now, uint32_t then) {
    return static_cast<uint32_t>(now - then);
  }

  void clearRuntime() {
    ready_ = false;
    inputModeRequested_ = false;
    previousActive_ = false;
    haveSample_ = false;
    haveAttempt_ = false;
    lastSampleMs_ = lastAttemptMs_ = 0;
    pending_ = deferred_ = false;
    pendingState_ = deferredState_ = false;
    coalescedEvents_ = 0;
  }

  static bool sameConfig(const Config &a, const Config &b) {
    return a.enabled == b.enabled && a.monitorPin == b.monitorPin &&
      a.usePullup == b.usePullup &&
      a.minimumBroadcastSecs == b.minimumBroadcastSecs &&
      a.stateBroadcastSecs == b.stateBroadcastSecs &&
      a.sendBell == b.sendBell && a.trigger == b.trigger &&
      memcmp(a.name, b.name, NameCapacity) == 0;
  }

  static bool sameAction(EventKind kind, bool state, EventKind otherKind,
                         bool otherState) {
    return kind == otherKind && state == otherState;
  }

  void queue(EventKind kind, bool state, Cadence cadence) {
    if (!pending_) {
      pending_ = true; pendingKind_ = kind; pendingState_ = state;
      pendingCadence_ = cadence; return;
    }
    if (sameAction(kind, state, pendingKind_, pendingState_)) return;
    if (!deferred_) {
      deferred_ = true; deferredKind_ = kind; deferredState_ = state;
      deferredCadence_ = cadence; return;
    }
    if (!sameAction(kind, state, deferredKind_, deferredState_) &&
        coalescedEvents_ != UINT32_MAX) ++coalescedEvents_;
  }

  void promoteDeferred() {
    if (!deferred_) { pending_ = false; return; }
    pending_ = true; pendingKind_ = deferredKind_;
    pendingState_ = deferredState_; pendingCadence_ = deferredCadence_;
    deferred_ = false;
  }

  bool due(uint32_t now, Cadence cadence) const {
    const uint32_t seconds = cadence == Cadence::MinimumInterval
        ? minimumSecs(config_) : config_.stateBroadcastSecs;
    if (seconds == 0) return false;
    const uint32_t interval = seconds * 1000u;
    // Stock millis/Throttle behavior uses unsigned subtraction and is wrap-safe
    // for the bounded intervals accepted by validConfig().
    return elapsed(now, haveAttempt_ ? lastAttemptMs_ : 0u) >= interval;
  }

  bool compose(Event &event) const {
    const auto verdict = event.kind == EventKind::Detection ? stock::detection::Detected :
                                                              stock::detection::SendState;
    event.payloadSize = stock::detection::composeMessage(event.payload, sizeof(event.payload),
                                                         config_.name, verdict, event.state, config_.sendBell);
    return event.payloadSize != 0;
  }

public:
  ConfigureResult configure(const Config &candidate, bool boardPinResolved) {
    if (!validConfig(candidate)) return ConfigureResult::Invalid;
    const bool available = !candidate.enabled ||
        (candidate.monitorPin > 0 && boardPinResolved);
    if (ready_ && sameConfig(config_, candidate) && available)
      return candidate.enabled ? ConfigureResult::Accepted : ConfigureResult::Disabled;
    config_ = candidate;
    clearRuntime();
    if (!candidate.enabled) return ConfigureResult::Disabled;
    if (!available) return ConfigureResult::UnavailablePin;
    ready_ = true;
    return ConfigureResult::Accepted;
  }

  bool enabledAndReady() const { return config_.enabled && ready_; }
  uint32_t coalescedEvents() const { return coalescedEvents_; }

  template <class Sink>
  PollResult poll(uint32_t nowMs, bool rawLevel, bool defaultChannel, Sink &&sink) {
    PollResult result{};
    result.coalescedEvents = coalescedEvents_;
    if (!config_.enabled) { result.code = PollCode::Disabled; return result; }
    if (!ready_) { result.code = PollCode::Unavailable; return result; }
    if (!inputModeRequested_) {
      inputModeRequested_ = true;
      result.code = PollCode::InputModeRequested;
      result.requestInputMode = true;
      result.pullup = config_.usePullup;
      return result;
    }
    if (haveSample_ && elapsed(nowMs, lastSampleMs_) < SampleIntervalMs) {
      result.code = PollCode::NotSampleDue; return result;
    }
    haveSample_ = true; lastSampleMs_ = nowMs;
    result.sampled = true;

    const bool active = stock::detection::hasDetectionEvent(config_.trigger, rawLevel);
    const bool minimumDue = due(nowMs, Cadence::MinimumInterval);
    const bool retryEdge = minimumDue && ((pending_ && pendingCadence_ == Cadence::MinimumInterval) ||
                                          (deferred_ && deferredCadence_ == Cadence::MinimumInterval));
    const auto sample = stock::detection::runOnce(config_.trigger, previousActive_, active, minimumDue,
                                                  due(nowMs, Cadence::StateHeartbeat), retryEdge);
    if (sample.verdict != stock::detection::Noop)
      queue(sample.verdict == stock::detection::Detected ? EventKind::Detection : EventKind::State,
            active, sample.heartbeat ? Cadence::StateHeartbeat : Cadence::MinimumInterval);

    bool selectedDeferred = false;
    if (pending_ && due(nowMs, pendingCadence_)) {
      selectedDeferred = false;
    } else if (deferred_ && due(nowMs, deferredCadence_)) {
      selectedDeferred = true;
    } else {
      result.code = PollCode::Sampled; result.coalescedEvents = coalescedEvents_;
      return result;
    }

    Event event{};
    event.kind = selectedDeferred ? deferredKind_ : pendingKind_;
    event.state = selectedDeferred ? deferredState_ : pendingState_;
    if (!compose(event)) {
      result.code = PollCode::Unavailable; result.coalescedEvents = coalescedEvents_;
      return result;
    }
    // Stock advances the send throttle before checking the primary channel or
    // handing the packet to Service. Preserve that cadence, but keep the same
    // bounded pending action on callback refusal so retries are visible and
    // cadence-limited rather than silently lost or retried every sample.
    haveAttempt_ = true; lastAttemptMs_ = nowMs;
    if (defaultChannel) {
      if (selectedDeferred) deferred_ = false;
      else promoteDeferred();
      result.code = PollCode::PublicChannelBlocked;
      result.coalescedEvents = coalescedEvents_;
      return result;
    }
    if (!sink(event)) {
      result.code = PollCode::Backpressured; result.hasEvent = true;
      result.event = event; result.coalescedEvents = coalescedEvents_;
      return result;
    }
    if (selectedDeferred) deferred_ = false;
    else promoteDeferred();
    result.code = PollCode::Published; result.hasEvent = true;
    result.event = event; result.coalescedEvents = coalescedEvents_;
    return result;
  }
};

} // namespace mt::modules::detection
