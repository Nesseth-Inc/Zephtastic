// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "tx_window.h"
#include <cstdint>
#include "Observed.h"

namespace mt {
// Trusted monotonic64-bit uptime. UINT64_MAX means unavailable.
using UptimeMsProvider=uint64_t (*)(void *);
struct UptimeClock {UptimeMsProvider get;void *ctx;};

// Estimated airtime of observed clean completions. Uncertain/lost events hide
// the field for a whole hour after their trustworthy terminal time, or after
// the first trustworthy later observation when that time was unavailable.
// The counter retains absolute-minute stock quantization. Quality suppression
// is deliberately stricter than stock fault-path accounting. No RF gate changes.
class AirtimeMeter {
 public:
  bool bind(UptimeClock clock) {
    if(bound_||!clock.get)return false;
    const uint64_t now=clock.get(clock.ctx);
    if(now==UINT64_MAX)return false;
    clock_=clock;bound_=true;
    if(pending_)taint({});
    lastClock_=now;haveClock_=true;
    resolveTaintTime(now);
    // Explicit known idle, rather than a fallback for a failed/absent reading.
    if(window_.record(now,0)!=zt::TxMinuteWindow::Error::None) {
      increment(lost_);taint(now);return false;
    }
    return true;
  }
  bool bound()const{return bound_;}
  void ensureAnchored() {
    if(!bound_||window_.anchored())return;
    auto now=readClock();
    if(now&&window_.record(*now,0)!=zt::TxMinuteWindow::Error::None) {
      increment(lost_);taint(now);
    }
  }
  void noteAcceptedStart(uint32_t estimatedMs) {
    // A lost terminal event cannot be hidden by a second start.
    if(pending_)noteUncertain();
    pending_=true;pendingMs_=estimatedMs;
  }
  void creditPending() {
    if(!pending_)return;
    pending_=false;
    auto now=readClock();
    if(!now) {increment(lost_);taint({});return;}
    if(window_.record(*now,pendingMs_)!=zt::TxMinuteWindow::Error::None) {
      increment(lost_);taint(now);
    }
  }
  void noteUncertain() {
    if(!pending_)return;
    pending_=false;increment(uncertain_);
    taint(readClock());
  }
  bool pending()const{return pending_;}
  uint32_t uncertain()const{return uncertain_;}
  uint32_t lost()const{return lost_;}
  bool anchored()const{return window_.anchored();}
  // Read-time aging mutates the window and remains under Service's owner lock.
  zt::Observed<float> percent() {
    auto now=readClock();
    if(!now||!window_.anchored())return {};
    if(tainted_) {
      if(!taintTimeKnown_||*now<taintAt_||*now-taintAt_<HourMs)return {};
      tainted_=false;
    }
    auto value=window_.percentAt(*now);
    if(!value) {increment(lost_);taint(now);}
    return value;
  }
  const zt::TxMinuteWindow &window()const{return window_;}
 private:
  static constexpr uint64_t HourMs=3600000;
  static void increment(uint32_t &value){if(value<UINT32_MAX)++value;}
  void taint(zt::Observed<uint64_t> now) {
    tainted_=true;taintTimeKnown_=now.has_value();
    if(now)taintAt_=*now;
  }
  void resolveTaintTime(uint64_t now) {
    if(tainted_&&!taintTimeKnown_){taintAt_=now;taintTimeKnown_=true;}
  }
  zt::Observed<uint64_t> readClock() {
    if(!bound_)return {};
    const uint64_t now=clock_.get(clock_.ctx);
    if(now==UINT64_MAX||(haveClock_&&now<lastClock_)) {
      taint({});return {};
    }
    lastClock_=now;haveClock_=true;resolveTaintTime(now);
    return now;
  }
  zt::TxMinuteWindow window_;
  UptimeClock clock_{};
  uint64_t lastClock_=0,taintAt_=0;
  uint32_t pendingMs_=0,uncertain_=0,lost_=0;
  bool bound_=false,pending_=false,haveClock_=false,tainted_=false,taintTimeKnown_=false;
};
}  // namespace mt
