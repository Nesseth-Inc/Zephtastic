// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../../stock/peripherals/AmbientLightingThread.h"

namespace mt::modules::peripherals {

// Allocation-free policy core corresponding to Meshtastic's
// AmbientLightingThread. Hardware access is deliberately supplied by caller.
class AmbientLightingCore {
public:
  using Config = stock::ambient::Config;
  using Apply = stock::ambient::SetLighting;
  using Off = int (*)(void *context);
  using Result = stock::ambient::Result;

  void configure(const Config &config) { config_ = config; due_ = true; }
  Result poll(uint32_t nowMs, bool capable, void *context, Apply apply, Off off) {
    if (config_.ledState && capable && apply && !due_ &&
        static_cast<uint32_t>(nowMs - lastApplyMs_) < RefreshMs)
      return Result::NotDue;
    const Result result = stock::ambient::runOnce(config_, capable, apply, context);
    if (result != Result::Applied) {
      if (result != Result::Failed && wasOn_) {
        if (off) off(context);
        wasOn_ = false;
        due_ = true;
      }
      return result;
    }
    lastApplyMs_ = nowMs;
    wasOn_ = true;
    due_ = false;
    return Result::Applied;
  }
  static constexpr uint32_t RefreshMs = stock::ambient::RefreshMs;

private:
  Config config_{};
  uint32_t lastApplyMs_ = 0;
  bool due_ = true;
  bool wasOn_ = false;
};
} // namespace mt::modules::peripherals
