// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <meshtastic/remote_hardware.pb.h>
#include <stdint.h>
#include "../stock/modules/RemoteHardwareModule.h"

namespace mt::modules {

// Pure command policy. This class never talks to GPIO, Zephyr, a host process,
// or radio. The caller supplies masks derived from a reviewed board map.
class RemoteHardwarePolicy {
public:
  enum class Operation : uint8_t { None, Read, Write, Watch, Ignore };
  enum class Status : uint8_t {
    Accepted, Disabled, WrongChannel, InvalidCommand, InvalidCapability,
    EmptyMask, PartialAuthorization, NotReadable, NotWritable
  };

  struct PinDomain {
    // Pins physically exported by the selected board's reviewed alias table.
    uint64_t exportedMask;
    // Electrical/circuit capabilities, always constrained by exportedMask.
    uint64_t readableMask;
    uint64_t writableMask;
    // Per-configured-pin exposure allowlist. It can never add pins outside
    // exportedMask. With allowUndefined=true, the entire exported domain is
    // considered, still subject to readableMask/writableMask.
    uint64_t configuredMask;
  };

  struct Decision {
    Status status;
    Operation operation;
    uint64_t requestMask;
    uint64_t authorizedMask;
    uint64_t deniedMask;
    uint64_t writeValue;
    Decision() : status(Status::InvalidCommand), operation(Operation::None),
                 requestMask(0), authorizedMask(0), deniedMask(0), writeValue(0) {}
    bool accepted() const { return status == Status::Accepted; }
  };

  struct WatchEvent {
    meshtastic_HardwareMessage message;
    uint32_t observedAtMs;
    WatchEvent() : message(meshtastic_HardwareMessage_init_default), observedAtMs(0) {}
  };

  static constexpr uint32_t WatchIntervalMs = stock::RemoteHardwareModule::WatchIntervalMs;

  explicit RemoteHardwarePolicy(bool enabled = false)
      : enabled_(enabled), watchMask_(0), previousValue_(0), lastWatchMs_(0), initialPending_(false) {}

  void setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) clearWatch();
  }
  bool enabled() const { return enabled_; }
  uint64_t watchMask() const { return watchMask_; }

  // gpioChannel must be true only after the caller resolves the exact named
  // `Channels::gpioChannel` binding. It is not a peer-authentication result.
  Decision evaluate(const meshtastic_HardwareMessage &message, bool gpioChannel,
                    bool allowUndefinedPinAccess, const PinDomain &domain) {
    Decision d{};
    d.requestMask = message.gpio_mask;
    if (!enabled_) { d.status = Status::Disabled; return d; }
    if (!gpioChannel) { d.status = Status::WrongChannel; return d; }
    if (!validDomain(domain)) { d.status = Status::InvalidCapability; return d; }

    const uint32_t type = enumValue(message.type);
    if (type == uint32_t(meshtastic_HardwareMessage_Type_GPIOS_CHANGED) ||
        type == uint32_t(meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY)) {
      d.status = Status::Accepted; d.operation = Operation::Ignore; return d;
    }
    if (type != uint32_t(meshtastic_HardwareMessage_Type_READ_GPIOS) &&
        type != uint32_t(meshtastic_HardwareMessage_Type_WRITE_GPIOS) &&
        type != uint32_t(meshtastic_HardwareMessage_Type_WATCH_GPIOS)) {
      d.status = Status::InvalidCommand; return d;
    }
    if (type == uint32_t(meshtastic_HardwareMessage_Type_WATCH_GPIOS) && message.gpio_mask == 0) {
      clearWatch();
      d.status = Status::Accepted; d.operation = Operation::Watch; d.authorizedMask = 0; return d;
    }
    if (message.gpio_mask == 0) { d.status = Status::EmptyMask; return d; }

    const uint64_t exposure = allowUndefinedPinAccess ? domain.exportedMask
                                                       : (domain.configuredMask & domain.exportedMask);
    const uint64_t readAllowed = exposure & domain.readableMask;
    const uint64_t writeAllowed = exposure & domain.writableMask;
    uint64_t allowed = 0;
    Operation operation = Operation::None;
    if (type == uint32_t(meshtastic_HardwareMessage_Type_READ_GPIOS)) {
      allowed = readAllowed; operation = Operation::Read;
    } else if (type == uint32_t(meshtastic_HardwareMessage_Type_WRITE_GPIOS)) {
      allowed = writeAllowed; operation = Operation::Write;
    } else {
      allowed = readAllowed; operation = Operation::Watch;
    }
    d.authorizedMask = message.gpio_mask & allowed;
    d.deniedMask = message.gpio_mask & ~allowed;
    if (d.deniedMask) {
      d.status = d.deniedMask == message.gpio_mask
                   ? (operation == Operation::Write ? Status::NotWritable : Status::NotReadable)
                   : Status::PartialAuthorization;
      // Whole command denied; caller must do no I/O even when some bits pass.
      return d;
    }
    d.status = Status::Accepted; d.operation = operation;
    d.writeValue = message.gpio_value & message.gpio_mask;
    if (operation == Operation::Watch) {
      watchMask_ = message.gpio_mask;
      previousValue_ = 0;
      lastWatchMs_ = 0;
      initialPending_ = true;
    }
    return d;
  }

  // Build the stock READ_GPIOS_REPLY shape after the caller samples the
  // authorized pins. `sampled` is masked defensively before leaving policy.
  static bool makeReadReply(const Decision &decision, uint64_t sampled,
                            meshtastic_HardwareMessage &reply) {
    if (!decision.accepted() || decision.operation != Operation::Read) return false;
    stock::RemoteHardwareModule::readReply(decision.requestMask, sampled & decision.authorizedMask, reply);
    return true;
  }

  // Preserve stock's 30-second change throttle and broadcast message shape.
  // The stock producer leaves gpio_mask zero on GPIOS_CHANGED; this helper
  // mirrors that shape. Caller performs the actual polling and radio send.
  bool pollWatchValue(uint64_t sampled, uint32_t nowMs, WatchEvent &event) {
    if (!stock::RemoteHardwareModule::runOnce(enabled_, watchMask_, nowMs, sampled,
          lastWatchMs_, previousValue_, initialPending_, event.message)) return false;
    event.observedAtMs = nowMs;
    return true;
  }

  void clearWatch() {
    watchMask_ = 0; previousValue_ = 0; lastWatchMs_ = 0; initialPending_ = false;
  }

  static bool validDomain(const PinDomain &d) {
    return !(d.readableMask & ~d.exportedMask) &&
           !(d.writableMask & ~d.exportedMask) &&
           !(d.configuredMask & ~d.exportedMask);
  }

  // Safe Arduino-number-to-bit helper for future config adapters. Numeric pin
  // 64 and above are rejected before shifting; bit 63 is representable.
  static bool pinBit(unsigned pin, uint64_t &bit) {
    if (pin >= 64) { bit = 0; return false; }
    bit = UINT64_C(1) << pin;
    return true;
  }

private:
  template<class E> static uint32_t enumValue(const E &value) {
    using U = __underlying_type(E);
    // Read the stored representation without loading a possibly malformed
    // nanopb enum as an enum-typed value (which enum sanitizers trap first).
    U raw{}; __builtin_memcpy(&raw, &value, sizeof(raw));
    return static_cast<uint32_t>(raw);
  }
  bool enabled_;
  uint64_t watchMask_;
  uint64_t previousValue_;
  uint32_t lastWatchMs_;
  bool initialPending_;
};

} // namespace mt::modules
