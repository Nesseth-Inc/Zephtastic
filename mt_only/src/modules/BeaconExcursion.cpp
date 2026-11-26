// SPDX-License-Identifier: GPL-3.0-only
#include "BeaconExcursion.h"

#include <string.h>

namespace mt::modules {
BeaconExcursion::Action BeaconExcursion::action(ActionKind kind, uint32_t packetId,
    const RadioProfile *profile, bool legacyHopOverride) const
{
  Action result{};
  result.kind = kind;
  result.packetId = packetId;
  if (profile)
    result.profile = *profile;
  result.legacyHopOverride = legacyHopOverride;
  return result;
}

void BeaconExcursion::removeAt(size_t index)
{
  if (index >= count_)
    return;
  for (size_t i = index + 1; i < count_; ++i)
    queue_[i - 1] = queue_[i];
  --count_;
}

size_t BeaconExcursion::findPacket(uint32_t id) const
{
  for (size_t i = 0; i < count_; ++i)
    if (queue_[i].packetId == id)
      return i;
  return count_;
}

void BeaconExcursion::clearActive()
{
  active_ = Request{};
  haveActive_ = false;
  targetMayBeApplied_ = false;
  cancelRequested_ = false;
  restoreCurrentSettings_ = false;
  state_ = State::Idle;
}

bool BeaconExcursion::epochsMatch(uint32_t settingsEpoch, uint32_t identityEpoch) const
{
  return settingsEpoch == savedSettingsEpoch_ && identityEpoch == savedIdentityEpoch_;
}

BeaconExcursion::Action BeaconExcursion::staleAction(uint32_t id)
{
  const Action result = action(targetMayBeApplied_ ? ActionKind::ApplyCurrentSettings
                                                  : ActionKind::DropPacket, id);
  if (!targetMayBeApplied_)
    clearActive();
  return result;
}

BeaconExcursion::Action BeaconExcursion::restoreAction(bool applyCurrentSettings)
{
  state_ = State::Restoring;
  restoreCurrentSettings_ = applyCurrentSettings;
  return applyCurrentSettings
      ? action(ActionKind::ApplyCurrentSettings, active_.packetId)
      : action(ActionKind::RestoreHome, active_.packetId, &home_);
}

BeaconExcursion::Action BeaconExcursion::completeActive(uint32_t settingsEpoch,
                                                          uint32_t identityEpoch)
{
  const uint32_t id = active_.packetId;
  const bool stale = !epochsMatch(settingsEpoch, identityEpoch);
  clearActive();
  return action(stale ? ActionKind::ApplyCurrentSettings : ActionKind::None, id);
}

bool BeaconExcursion::enqueue(const Request &request)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  if (count_ == QueueCapacity || request.packetId == 0 || request.target.channelIndex>=8) {
    k_spin_unlock(&lock_, key);
    return false;
  }
  if (findPacket(request.packetId) != count_ || (haveActive_ && active_.packetId == request.packetId)) {
    k_spin_unlock(&lock_, key);
    return false;
  }
  queue_[count_++] = request;
  k_spin_unlock(&lock_, key);
  return true;
}

BeaconExcursion::Action BeaconExcursion::beforeTransmit(uint32_t id,
    const RadioProfile &home, uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  if (haveActive_) {
    const Action result = state_ == State::ReadyToTransmit && active_.packetId == id
        ? action(ActionKind::StartTransmission, id, nullptr, active_.legacyHopOverride)
        : action(ActionKind::HoldPacket, id);
    k_spin_unlock(&lock_, key);
    return result;
  }
  const size_t index = findPacket(id);
  if (index == count_) {
    const Action result = action(ActionKind::SendNormally, id);
    k_spin_unlock(&lock_, key);
    return result;
  }
  if (index != 0) {
    const Action result = action(ActionKind::HoldPacket, id);
    k_spin_unlock(&lock_, key);
    return result;
  }
  if (home.channelIndex>=8) {
    removeAt(index);
    const Action result = action(ActionKind::DropPacket, id);
    k_spin_unlock(&lock_, key);
    return result;
  }
  active_ = queue_[index];
  removeAt(index);
  haveActive_ = true;
  home_ = home;
  savedSettingsEpoch_ = settingsEpoch;
  savedIdentityEpoch_ = identityEpoch;
  rxEpochAtPrepare_ = rxEpoch_;
  cancelRequested_ = false;
  if (active_.settingsEpoch != settingsEpoch || active_.identityEpoch != identityEpoch) {
    clearActive();
    const Action result = action(ActionKind::DropPacket, id);
    k_spin_unlock(&lock_, key);
    return result;
  }
  if (!active_.requiresRetune || home_.sameRadio(active_.target)) {
    state_ = State::ReadyToTransmit;
    targetMayBeApplied_ = true;
    const Action result = action(ActionKind::StartTransmission, id, nullptr, active_.legacyHopOverride);
    k_spin_unlock(&lock_, key);
    return result;
  }
  state_ = State::NeedConfigure;
  const Action result = action(ActionKind::ConfigureTarget, id, &active_.target,
                               active_.legacyHopOverride);
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::beginConfigure(uint32_t id,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  Action result{};
  if (!haveActive_ || active_.packetId != id || state_ != State::NeedConfigure) {
    result = action(ActionKind::DropPacket, id);
  } else if (!epochsMatch(settingsEpoch, identityEpoch) || rxEpoch_ != rxEpochAtPrepare_) {
    result = staleAction(id);
  } else {
    state_ = State::Configuring;
    result = action(ActionKind::ConfigureTarget, id, &active_.target,
                    active_.legacyHopOverride);
  }
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::targetConfigured(uint32_t id, bool success,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  Action result{};
  if (!haveActive_ || active_.packetId != id || state_ != State::Configuring) {
    result = action(ActionKind::ApplyCurrentSettings, id);
  } else {
    targetMayBeApplied_ = success;
    if (!success) {
      clearActive();
      result = action(ActionKind::DropPacket, id);
    } else if (!epochsMatch(settingsEpoch, identityEpoch) || cancelRequested_ ||
               rxEpoch_ != rxEpochAtPrepare_) {
      result = restoreAction(!epochsMatch(settingsEpoch, identityEpoch));
    } else {
      state_ = State::ReadyToTransmit;
      result = action(ActionKind::StartTransmission, id, nullptr,
                      active_.legacyHopOverride);
    }
  }
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::beginTransmit(uint32_t id,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  Action result{};
  if (!haveActive_ || active_.packetId != id || state_ != State::ReadyToTransmit) {
    result = action(ActionKind::DropPacket, id);
  } else if (!epochsMatch(settingsEpoch, identityEpoch) || cancelRequested_ ||
             rxEpoch_ != rxEpochAtPrepare_) {
    result = targetMayBeApplied_ ? restoreAction(!epochsMatch(settingsEpoch, identityEpoch)) : staleAction(id);
  } else {
    state_ = State::InFlight;
    result = action(ActionKind::StartTransmission, id, nullptr,
                    active_.legacyHopOverride);
  }
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::transmitCompleted(uint32_t id,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  Action result{};
  if (!haveActive_ || active_.packetId != id || state_ != State::InFlight) {
    result = action(ActionKind::None, id);
  } else if (cancelRequested_ || !epochsMatch(settingsEpoch, identityEpoch) ||
             rxEpoch_ != rxEpochAtPrepare_) {
    result = restoreAction(!epochsMatch(settingsEpoch, identityEpoch));
  } else if (count_ != 0 && queue_[0].groupId != 0 &&
             queue_[0].groupId == active_.groupId &&
             queue_[0].target.sameRadio(active_.target) && queue_[0].target.channelIndex == active_.target.channelIndex &&
             queue_[0].settingsEpoch == settingsEpoch &&
             queue_[0].identityEpoch == identityEpoch) {
    // Stock FLAG_LEGACY_SPLIT sends the offer and text packets back-to-back on
    // one target. Keep the target profile until the second packet has finished.
    active_ = queue_[0];
    removeAt(0);
    state_ = State::ReadyToTransmit;
    result = action(ActionKind::StartTransmission, active_.packetId, nullptr,
                    active_.legacyHopOverride);
  } else {
    result = restoreAction();
  }
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::transmitFailed(uint32_t id,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  return transmitCompleted(id, settingsEpoch, identityEpoch);
}

BeaconExcursion::Action BeaconExcursion::restoreCompleted(bool success,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  Action result{};
  if (!haveActive_ || state_ != State::Restoring) {
    result = action(ActionKind::None);
  } else if (!success) {
    // Keep the radio held. The owner may retry restore; no subsequent packet may
    // be transmitted over an unknown target configuration.
    result = restoreCurrentSettings_
        ? action(ActionKind::ApplyCurrentSettings, active_.packetId)
        : action(ActionKind::RestoreHome, active_.packetId, &home_);
  } else {
    if (restoreCurrentSettings_) {
      savedSettingsEpoch_ = settingsEpoch;
      savedIdentityEpoch_ = identityEpoch;
      restoreCurrentSettings_ = false;
    }
    result = completeActive(settingsEpoch, identityEpoch);
  }
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::cancel(uint32_t id,
    uint32_t settingsEpoch, uint32_t identityEpoch)
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  Action result{};
  if (!haveActive_ || active_.packetId != id) {
    const size_t queuedIndex = findPacket(id);
    if (queuedIndex < count_)
      removeAt(queuedIndex);
    result = action(ActionKind::None, id);
  } else if (state_ == State::Configuring || state_ == State::InFlight) {
    cancelRequested_ = true;
    result = action(ActionKind::HoldPacket, id);
  } else if (targetMayBeApplied_) {
    result = restoreAction();
  } else {
    result = staleAction(id);
  }
  (void)settingsEpoch;
  (void)identityEpoch;
  k_spin_unlock(&lock_, key);
  return result;
}

BeaconExcursion::Action BeaconExcursion::receiveAccepted()
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  ++rxEpoch_;
  Action result = action(ActionKind::None);
  if (haveActive_) {
    if (state_ == State::NeedConfigure || state_ == State::ReadyToTransmit) {
      cancelRequested_ = true;
      result = targetMayBeApplied_ ? restoreAction()
                                  : staleAction(active_.packetId);
    } else if (state_ == State::Configuring || state_ == State::InFlight) {
      cancelRequested_ = true;
      result = action(ActionKind::HoldPacket, active_.packetId);
    }
  }
  k_spin_unlock(&lock_, key);
  return result;
}

bool BeaconExcursion::holdsRadio() const
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  const bool result = haveActive_;
  k_spin_unlock(&lock_, key);
  return result;
}

size_t BeaconExcursion::queued() const
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  const size_t result = count_;
  k_spin_unlock(&lock_, key);
  return result;
}

uint32_t BeaconExcursion::rxAcceptedEpoch() const
{
  k_spinlock_key_t key = k_spin_lock(&lock_);
  const uint32_t result = rxEpoch_;
  k_spin_unlock(&lock_, key);
  return result;
}
} // namespace mt::modules
