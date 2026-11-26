// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace mt {
enum class ScheduledOwnerAction : uint8_t { None, Reboot, FirmwareUpdate, Shutdown, Dfu };
// One volatile schedule shared by local transports, serialized by serviceLock.
// No timer/interrupt executes a reset while a settings write is in progress.
class RebootSchedule {
  uint64_t deadline_=0;
  bool pending_=false;
  ScheduledOwnerAction action_=ScheduledOwnerAction::None;
public:
  static constexpr uint64_t MinimumDelayMs=100;
  void request(int32_t seconds,uint64_t now,ScheduledOwnerAction action=ScheduledOwnerAction::Reboot) {
    if(seconds<0){clear();return;}
    action_=action;
    const uint64_t delay=seconds?uint64_t(seconds)*1000:MinimumDelayMs;
    deadline_=now+delay;pending_=true;
  }
  bool pending() const {return pending_;}
  // Shutdown and firmware handoff continue to own admission after the deadline
  // fires. Keep the action until its owner completes, cancels, or retries it.
  ScheduledOwnerAction action() const {return action_;}
  void clear(){pending_=false;action_=ScheduledOwnerAction::None;}
  bool takeDue(uint64_t now) {
    // Unsigned half-range comparison handles wrapping clocks; the largest
    // int32 seconds request is far below the 2^63 ms comparison horizon.
    if(!pending_||uint64_t(now-deadline_)>=(uint64_t(1)<<63))return false;
    pending_=false;return true;
  }
  ScheduledOwnerAction takeDueAction(uint64_t now) {
    return takeDue(now)?action_:ScheduledOwnerAction::None;
  }
  template<class Stop,class Reset> void poll(uint64_t now,Stop stop,Reset reset) {
    if(!takeDue(now))return;
    stop(); // Best effort: a stop failure must not strand an explicit reboot.
    reset();
  }
};
}
