// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

namespace mt {
namespace ble {

// Bounded callback-to-main/UI handoff for BLE pairing passkey events (M102).
//
// Concurrency contract:
//   Holds NO internal synchronization. The caller MUST hold the existing BLE
//   session lock (SessionLock) for every call. Pairing/session callbacks never
//   take the application service/UI lock (serviceLock); UI and controller
//   polling are serialized separately under the service lock.
//
// Scope:
//   Pure data handoff. Must NOT call Zephyr, UI, settings, logs, or USB.
//   Stores no timestamps, display strings, bond state, or peer identity.
//
// The session token is the nonzero BLE generation from M101 (BleTransport.cpp):
// incremented on connect, cleared on disconnect.
class BlePairingEvent {
 public:
  struct Snapshot {
    bool active = false;
    uint32_t passkey = 0;
    uint32_t session = 0;
  };

  // begin(0) clears all state. begin(nonzero) replaces the current session and
  // clears any passkey.
  void begin(uint32_t session) {
    session_ = session;
    passkey_ = 0;
    active_ = false;
  }

  // Publishes a passkey for the current session. Requires the current nonzero
  // session to match `session` and `passkey` to be within 0..999999. A
  // stale/zero session or out-of-range PIN returns false and changes nothing.
  bool publishPasskey(uint32_t session, uint32_t passkey) {
    if (session_ == 0 || session != session_ || passkey > 999999u) {
      return false;
    }
    passkey_ = passkey;
    active_ = true;
    return true;
  }

  // Clears the passkey. Succeeds idempotently for the current nonzero session
  // (returns true even if already cleared). A stale/zero session returns false
  // and changes nothing.
  bool clear(uint32_t session) {
    if (session_ == 0 || session != session_) {
      return false;
    }
    passkey_ = 0;
    active_ = false;
    return true;
  }

  // Reads the current state. For the current nonzero session it succeeds even
  // when inactive (so the UI can clear a prior display). A stale/zero session
  // returns false and sets `out` to the default so no old PIN leaks.
  bool snapshot(uint32_t session, Snapshot& out) const {
    if (session_ == 0 || session != session_) {
      out = Snapshot{};
      return false;
    }
    out.active = active_;
    out.passkey = passkey_;
    out.session = session_;
    return true;
  }

 private:
  // Current nonzero BLE session token (0 = no session).
  uint32_t session_ = 0;
  // Passkey, meaningful only when active_ is true.
  uint32_t passkey_ = 0;
  // Whether a passkey is currently published for session_. Tracked separately
  // because a valid PIN can be 0, which cannot be distinguished from "no PIN"
  // by passkey_ alone.
  bool active_ = false;
};

}  // namespace ble
}  // namespace mt
