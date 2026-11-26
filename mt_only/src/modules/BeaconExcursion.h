// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

namespace mt::modules {

// Resolves Meshtastic broadcast_targets without mutating settings or channel
// tables. The owner supplies channel-table lookup and stock preset display name.
class BeaconExcursion {
public:
  static constexpr size_t MaxTargets=4;
  static constexpr size_t QueueCapacity=8; // four targets, legacy split A+B
  // Settings and identity epochs protect these indices until completion.
  struct RadioProfile {
    uint32_t preset=0,region=0,slot=0;
    uint8_t channelIndex=0;
    bool usePreset=true;
    bool sameRadio(const RadioProfile &other) const {
      return region==other.region && preset==other.preset && usePreset==other.usePreset && slot==other.slot;
    }
  };
  struct ResolvedTarget {
    RadioProfile profile{};
    bool requiresRetune=false,legacyHopOverride=false;
  };

  enum class ActionKind : uint8_t {
    None, SendNormally, HoldPacket, ConfigureTarget, StartTransmission,
    RestoreHome, DropPacket, ApplyCurrentSettings
  };
  struct Action {
    ActionKind kind=ActionKind::None;
    uint32_t packetId=0;
    RadioProfile profile{};
    bool legacyHopOverride=false;
  };
  struct Request {
    uint32_t packetId=0;
    uint32_t groupId=0; // split offer/text pair shares one target group
    uint32_t settingsEpoch=0;
    uint32_t identityEpoch=0;
    RadioProfile target{};
    bool requiresRetune=false;
    bool legacyHopOverride=false;
  };

  // Queue one packet's immutable excursion request before radio admission.
  bool enqueue(const Request &request);
  // Called by the radio owner before admitting this packet. It captures a home
  // snapshot on the first target packet and returns an explicit action only.
  Action beforeTransmit(uint32_t packetId,const RadioProfile &home,
                        uint32_t settingsEpoch,uint32_t identityEpoch);
  // Gate the actual retune after the owner has received ConfigureTarget. A RX
  // accepted event or stale epoch between those calls prevents the retune.
  Action beginConfigure(uint32_t packetId,uint32_t settingsEpoch,uint32_t identityEpoch);
  Action targetConfigured(uint32_t packetId,bool success,
                          uint32_t settingsEpoch,uint32_t identityEpoch);
  Action beginTransmit(uint32_t packetId,uint32_t settingsEpoch,uint32_t identityEpoch);
  Action transmitCompleted(uint32_t packetId,uint32_t settingsEpoch,uint32_t identityEpoch);
  Action transmitFailed(uint32_t packetId,uint32_t settingsEpoch,uint32_t identityEpoch);
  Action restoreCompleted(bool success,uint32_t settingsEpoch,uint32_t identityEpoch);
  Action cancel(uint32_t packetId,uint32_t settingsEpoch,uint32_t identityEpoch);
  Action receiveAccepted();
  bool holdsRadio() const;
  size_t queued() const;
  uint32_t rxAcceptedEpoch() const;

private:
  enum class State : uint8_t { Idle, NeedConfigure, Configuring, ReadyToTransmit, InFlight, Restoring };
  Request queue_[QueueCapacity]{};
  size_t count_=0;
  Request active_{};
  RadioProfile home_{};
  State state_=State::Idle;
  uint32_t savedSettingsEpoch_=0, savedIdentityEpoch_=0, rxEpoch_=0, rxEpochAtPrepare_=0;
  bool haveActive_=false, targetMayBeApplied_=false, cancelRequested_=false;
  bool restoreCurrentSettings_=false;
  mutable struct k_spinlock lock_{};

  Action action(ActionKind kind,uint32_t packetId=0,const RadioProfile *profile=nullptr,
                bool legacyHopOverride=false) const;
  void removeAt(size_t index);
  size_t findPacket(uint32_t packetId) const;
  void clearActive();
  bool epochsMatch(uint32_t settingsEpoch,uint32_t identityEpoch) const;
  Action staleAction(uint32_t packetId);
  Action restoreAction(bool applyCurrentSettings=false);
  Action completeActive(uint32_t settingsEpoch,uint32_t identityEpoch);
};
} // namespace mt::modules
