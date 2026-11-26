// SPDX-License-Identifier: GPL-3.0-only
// PendingBluetoothAdmin.h: RAM-only staging for one pending Bluetooth settings commit; all methods run under the existing serviceLock.
#ifndef MT_ONLY_BLE_PENDING_BLUETOOTH_ADMIN_H_
#define MT_ONLY_BLE_PENDING_BLUETOOTH_ADMIN_H_

#include <stdint.h>
#include "BluetoothSettings.h"

namespace mt { namespace ble {

class PendingBluetoothAdmin {
 public:
  enum class State : uint8_t { Empty, Pending, Committing };

  struct Request {
    uint32_t session=0, packetId=0;
    bool wantAck=false, wantResponse=false;
    mt::BluetoothSettings desired;
  };

  struct Completion {
    uint32_t session=0, packetId=0;
    bool wantAck=false, wantResponse=false, replyAllowed=false;
    int result=0;
  };

  bool stage(const Request& in, bool hasDisplay) {
    if (state_ != State::Empty || in.session == 0 ||
        !mt::BluetoothSettings::valid(in.desired, hasDisplay)) return false;
    request_ = in;
    state_ = State::Pending;
    return true;
  }

  bool claim(uint32_t currentSession, Request& out) {
    if (state_ == State::Pending && currentSession != 0 &&
        currentSession == request_.session) {
      out = request_;
      state_ = State::Committing;
      return true;
    }
    if (state_ == State::Pending) { request_ = Request{}; state_ = State::Empty; }
    out = Request{};
    return false;
  }

  void disconnected(uint32_t session) {
    if (session == 0 || request_.session != session) return;
    if (state_ == State::Pending) { request_ = Request{}; state_ = State::Empty; }
    else if (state_ == State::Committing) { disconnected_ = true; }
  }

  bool finish(uint32_t currentSession, int result, Completion& out) {
    if (state_ != State::Committing) { out = Completion{}; return false; }
    out.session = request_.session;
    out.packetId = request_.packetId;
    out.wantAck = request_.wantAck;
    out.wantResponse = request_.wantResponse;
    out.result = result;
    out.replyAllowed = currentSession != 0 &&
        currentSession == request_.session && !disconnected_;
    request_ = Request{};
    disconnected_ = false;
    state_ = State::Empty;
    return true;
  }

  bool busy() const { return state_ != State::Empty; }
  State state() const { return state_; }

 private:
  State state_ = State::Empty;
  Request request_;
  bool disconnected_ = false;
};

} } // namespace mt::ble

#endif // MT_ONLY_BLE_PENDING_BLUETOOTH_ADMIN_H_
