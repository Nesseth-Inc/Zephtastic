// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "AirtimeMeter.h"
#include "ChannelWindow.h"
#include "ChannelTrace.h"

namespace mt {
class ChannelMeter {
 public:
  using Drain=void (*)(ChannelMeter &,void *);
  bool bind(UptimeClock clock,Drain drain=nullptr,void *ctx=nullptr) {
    if(bound_||!clock.get)return false;
    clock_=clock;drain_=drain;drainCtx_=ctx;bound_=true;return true;
  }
  void supported(bool value) {
    if(supported_&&!value)uncertain(zt::ChannelEvent::Support);
    if(value&&!supported_&&window_.anchored())uncertain(zt::ChannelEvent::Support);
    supported_=value;
    if(value&&bound_&&!window_.anchored())(void)window_.record(now(),0);
  }
  void observeRx(uint32_t milliseconds,bool known,uint16_t irq=0,uint8_t length=0) {
    zt::noteChannelTrace(trace_,known?zt::ChannelEvent::RxKnown:zt::ChannelEvent::RxUnknown,milliseconds,irq,length);
    if(!known){uncertain(zt::ChannelEvent::None);return;}
    (void)window_.record(now(),milliseconds);
  }
  void uncertain(zt::ChannelEvent reason=zt::ChannelEvent::Other){
    zt::noteChannelTrace(trace_,reason);window_.invalidate(now());
  }
  void noteAcceptedStart(uint32_t milliseconds) {
    if(pending_)uncertain(zt::ChannelEvent::PendingOverlap);
    pending_=true;pendingMs_=milliseconds;
  }
  void creditPending() {
    if(!pending_)return;
    pending_=false;(void)window_.record(now(),pendingMs_);
  }
  void noteUncertain() {
    if(!pending_)return;
    pending_=false;uncertain(zt::ChannelEvent::TxUncertain);
  }
  zt::Observed<float> percent() {
    // The caller already owns Service serialization. The drain only consumes
    // scalar queue metadata; it never calls a modem API or takes that lock.
    if(drain_)drain_(*this,drainCtx_);
    if(!bound_){lastAvailable_=false;return {};}
    auto value=window_.loggedPercentAt(now());lastAvailable_=value.has_value();return value;
  }
  zt::Observed<float> smoothedPercent() {
    if(drain_)drain_(*this,drainCtx_);
    return bound_ ? window_.smoothedLoggedPercentAt(now()) : zt::Observed<float>{};
  }
  const zt::ChannelTrace &diagnosticTrace()const{return trace_;}
  bool diagnosticSupported()const{return supported_;}
  bool diagnosticPending()const{return pending_;}
  bool diagnosticBound()const{return bound_;}
  bool diagnosticLastAvailable()const{return lastAvailable_;}
 private:
  zt::ChannelTrace trace_{};
  bool lastAvailable_=false;
  uint64_t now(){return bound_?clock_.get(clock_.ctx):UINT64_MAX;}
  zt::ChannelWindow window_;
  UptimeClock clock_{};
  Drain drain_=nullptr;
  void *drainCtx_=nullptr;
  uint32_t pendingMs_=0;
  bool bound_=false,supported_=false,pending_=false;
};
}
