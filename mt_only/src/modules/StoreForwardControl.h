// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <new>
#include <string.h>

#include "StoreForwardArchive.h"
#include "../stock/modules/StoreForwardModule.h"
#include "StoreForwardWire.h"

namespace mt::modules {

// Pure Store & Forward control/scheduling policy distilled from the pinned
// stock StoreForwardModule. The caller must authenticate the packet source,
// validate decoded STORE_FORWARD_APP provenance, choose the stock server role,
// and supply the default/public-channel decision before dispatch. This class
// performs no radio, packet allocation, encryption, settings, or heap work.
class StoreForwardControl {
  friend struct stock::StoreForwardModule;
public:
  static constexpr uint32_t DefaultReturnMax = 25;
  static constexpr uint32_t DefaultWindowMinutes = 240;
  static constexpr uint32_t DefaultHeartbeatSeconds = 900;
  static constexpr uint32_t PacketIntervalMs = 5000;
  static constexpr uint32_t DefaultHeartbeatPeriodSeconds = 900;
  static constexpr uint32_t BroadcastNode = StoreForwardArchive::BroadcastNode;

  enum class ActionKind : uint8_t {
    None,
    ControlMessage,
    ErrorText,
    Replay,
    ReplayFinished,
    Cancelled,
  };
  enum class ErrorTextKind : uint8_t { Busy, PublicChannel };

  struct Config {
    bool enabled;
    bool isServer;
    bool heartbeat;
    uint32_t heartbeatPeriodSeconds;
    uint32_t returnMax;
    uint32_t returnWindowMinutes;
    constexpr Config(bool e=false, bool server=false, bool hb=false,
                     uint32_t hbSeconds=DefaultHeartbeatPeriodSeconds,
                     uint32_t max=DefaultReturnMax,
                     uint32_t window=DefaultWindowMinutes)
        : enabled(e), isServer(server), heartbeat(hb),
          heartbeatPeriodSeconds(hbSeconds), returnMax(max),
          returnWindowMinutes(window) {}
  };

  struct Context {
    uint32_t peer = 0;
    uint32_t nowMillis = 0;
    uint32_t nowEpochSeconds = 0;
    bool defaultChannel = false;
    bool channelUtilAllowed = false;
    bool wantResponse = false;
  };

  struct Action {
    ActionKind kind;
    uint32_t destination;
    ErrorTextKind errorText;
    // The mutually exclusive protocol response or replay record shares one
    // fixed-size payload slot. Read only the member selected by `kind`.
    union {
      meshtastic_StoreAndForward control;
      StoreForwardArchive::Record replay;
    };
    uint32_t requestWindowSeconds;
    bool wantResponse;
    bool countsAsResponse;
    bool finalReplay;

    Action()
        : kind(ActionKind::None), destination(0),
          errorText(ErrorTextKind::Busy),
          control(meshtastic_StoreAndForward_init_zero),
          requestWindowSeconds(0), wantResponse(false),
          countsAsResponse(false), finalReplay(false) {}
    void activateReplay() {
      new (&replay) StoreForwardArchive::Record{};
    }
  };

private:
  StoreForwardArchive &archive_;
  Config config_;
  bool active_ = false;
  uint32_t requester_ = 0;
  uint32_t cutoff_ = 0;
  uint32_t requestWindowSeconds_ = 0;
  uint32_t planned_ = 0;
  uint32_t selected_ = 0;
  uint32_t dueMillis_ = 0;
  uint32_t requests_ = 0;
  uint32_t historyRequests_ = 0;
  uint32_t lastHeartbeatMillis_ = 0;
  uint32_t heartbeatPeriodSeconds_ = DefaultHeartbeatPeriodSeconds;
  uint32_t observedHeartbeatPeriodSeconds_ = DefaultHeartbeatPeriodSeconds;

  static Config normalize(Config config) {
    if (!config.heartbeatPeriodSeconds)
      config.heartbeatPeriodSeconds = DefaultHeartbeatPeriodSeconds;
    if (!config.returnMax) config.returnMax = DefaultReturnMax;
    if (!config.returnWindowMinutes)
      config.returnWindowMinutes = DefaultWindowMinutes;
    return config;
  }

  static bool due(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
  }
  static void setControl(Action &action, uint32_t destination,
                         meshtastic_StoreAndForward_RequestResponse rr) {
    action.kind = ActionKind::ControlMessage;
    action.destination = destination;
    action.control = meshtastic_StoreAndForward_init_zero;
    action.control.rr = rr;
  }
  void cancel() {
    active_ = false;
    planned_ = selected_ = 0;
  }

public:
  // Class-local placement construction, as in the existing private runtime;
  // supported by the pinned minimal C++ library without enabling heap new.
  static void *operator new(size_t,void *storage)noexcept{return storage;}
  explicit StoreForwardControl(StoreForwardArchive &archive,
                               const Config &config = Config{})
      : archive_(archive), config_(normalize(config)),
        heartbeatPeriodSeconds_(config_.heartbeatPeriodSeconds) {}

  bool active() const { return active_; }
  uint32_t activeRequester() const { return active_ ? requester_ : 0; }
  uint32_t requests() const { return requests_; }
  uint32_t historyRequests() const { return historyRequests_; }

  void configure(const Config &config) {
    config_ = normalize(config);
    heartbeatPeriodSeconds_ = config_.heartbeatPeriodSeconds;
    if (!config_.enabled || !config_.isServer) cancel();
  }

  // Process a decoded port-65 message after the caller has rejected packets
  // from this node and established the sender identity/channel profile.
  // Stock increments the aggregate request counter for every decoded request.
  Action receive(const meshtastic_StoreAndForward &message,
                 const Context &context) {
    Action action{};
    action.destination = context.peer;
    stock::StoreForwardModule::handleReceivedProtobuf(*this, message, context, action);
    return action;
  }

  // One scheduler call selects at most one replay. `channelUtilAllowed` is the
  // caller's stock `isTxAllowedChannelUtil(true)` decision (25% channel limit).
  // Selection advances the archive cursor before delivery just as stock does
  // after packet allocation; integration must not report this as TX success.
  Action poll(uint32_t nowMillis, bool channelUtilAllowed) {
    Action action{};
    stock::StoreForwardModule::runOnce(*this, nowMillis, channelUtilAllowed, action);
    return action;
  }

  // Call after a final replay action has been emitted or cannot be scheduled.
  void finishReplay() { cancel(); }

  // Client-side heartbeat observation uses stock period field only when the
  // heartbeat oneof is present; PONG also refreshes liveness without a period.
  void observeRouterHeartbeat(const meshtastic_StoreAndForward &message,
                              uint32_t nowMillis) {
    stock::StoreForwardModule::observeRouterHeartbeat(message, nowMillis, lastHeartbeatMillis_,
                                                    observedHeartbeatPeriodSeconds_);
  }
  uint32_t heartbeatPeriodSeconds() const { return heartbeatPeriodSeconds_; }
  uint32_t observedHeartbeatPeriodSeconds() const {
    return observedHeartbeatPeriodSeconds_;
  }
  uint32_t lastHeartbeatMillis() const { return lastHeartbeatMillis_; }
};

static_assert(sizeof(StoreForwardControl::Action) <= 320,
              "bounded action storage for control or one replay record");

}  // namespace mt::modules
