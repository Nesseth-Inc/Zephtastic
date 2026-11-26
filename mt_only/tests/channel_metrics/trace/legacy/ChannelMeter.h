// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "AirtimeMeter.h"
#include "ChannelWindow.h"

namespace mt {
class ChannelMeter {
 public:
  using Drain=void (*)(ChannelMeter &,void *);
  bool bind(UptimeClock clock,Drain drain=nullptr,void *ctx=nullptr) {
    if(bound_||!clock.get)return false;
    clock_=clock;drain_=drain;drainCtx_=ctx;bound_=true;return true;
  }
  void supported(bool value) {
    if(supported_&&!value)uncertain();
    if(value&&!supported_&&window_.anchored())uncertain();
    supported_=value;
    if(value&&bound_&&!window_.anchored())(void)window_.record(now(),0);
  }
  void observeRx(uint32_t milliseconds,bool known) {
    if(!known){uncertain();return;}
    (void)window_.record(now(),milliseconds);
  }
  void uncertain(){window_.invalidate(now());}
  void noteAcceptedStart(uint32_t milliseconds) {
    if(pending_)uncertain();
    pending_=true;pendingMs_=milliseconds;
  }
  void creditPending() {
    if(!pending_)return;
    pending_=false;(void)window_.record(now(),pendingMs_);
  }
  void noteUncertain() {
    if(!pending_)return;
    pending_=false;uncertain();
  }
  zt::Observed<float> percent() {
    // The caller already owns Service serialization. The drain only consumes
    // scalar queue metadata; it never calls a modem API or takes that lock.
    if(drain_)drain_(*this,drainCtx_);
    if(!bound_||!supported_)return {};
    return window_.percentAt(now());
  }
 private:
  uint64_t now(){return bound_?clock_.get(clock_.ctx):UINT64_MAX;}
  zt::ChannelWindow window_;
  UptimeClock clock_{};
  Drain drain_=nullptr;
  void *drainCtx_=nullptr;
  uint32_t pendingMs_=0;
  bool bound_=false,supported_=false,pending_=false;
};
}
