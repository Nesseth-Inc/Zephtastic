// SPDX-License-Identifier: GPL-3.0-only
// BleReplyProgress.h - RAM-only contiguous read coverage tracker for a
// BLE reply drain window. Caller serializes access. Tracks observed
// contiguous read coverage only (not peer receipt, security, durability).
#ifndef MT_BLE_REPLY_PROGRESS_H
#define MT_BLE_REPLY_PROGRESS_H

#include <stdint.h>
#include <stddef.h>

namespace mt {
namespace ble {

class BleReplyProgress {
public:
    enum class State : uint8_t { Idle, Waiting, Drained, Expired, Cancelled };

    bool begin(uint32_t session, size_t length, uint64_t now, uint32_t timeoutMs) {
        if (state_ != State::Idle) return false;
        if (session == 0u) return false;
        if (length < 1u || length > 512u) return false;
        if (timeoutMs < 1u || timeoutMs > 10000u) return false;
        if (now > UINT64_MAX - static_cast<uint64_t>(timeoutMs)) return false;
        session_ = session;
        length_ = length;
        start_ = now;
        deadline_ = now + static_cast<uint64_t>(timeoutMs);
        covered_ = 0u;
        state_ = State::Waiting;
        return true;
    }

    bool read(uint32_t session, size_t offset, size_t count, uint64_t now) {
        if (state_ != State::Waiting) return false;
        if (session == 0u || session != session_) return false;
        if (now < start_ || now >= deadline_) { state_ = State::Expired; return false; }
        if (count == 0u) return false;
        if (offset > covered_) return false;
        if (offset > length_) return false;
        if (count > length_ - offset) return false;
        const size_t end = offset + count;
        if (end > covered_) covered_ = end;
        if (covered_ == length_) state_ = State::Drained;
        return true;
    }

    State poll(uint64_t now) {
        if (state_ == State::Waiting && (now < start_ || now >= deadline_))
            state_ = State::Expired;
        return state_;
    }

    void cancel(uint32_t session) {
        if (state_ == State::Waiting && session != 0u && session == session_)
            state_ = State::Cancelled;
    }

    void reset() {
        state_ = State::Idle;
        session_ = 0u;
        length_ = 0u;
        start_ = 0u;
        deadline_ = 0u;
        covered_ = 0u;
    }

    State state() const { return state_; }
    size_t covered() const { return covered_; }

private:
    State state_ = State::Idle;
    uint32_t session_ = 0u;
    size_t length_ = 0u;
    uint64_t start_ = 0u;
    uint64_t deadline_ = 0u;
    size_t covered_ = 0u;
};

}  // namespace ble
}  // namespace mt

#endif  // MT_BLE_REPLY_PROGRESS_H
