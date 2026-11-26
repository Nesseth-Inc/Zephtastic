// SPDX-License-Identifier: GPL-3.0-only
// Object calculation from Meshtastic's pinned EmotiBit/SparkFun MLX90632
// v1.0.8. Register I/O and measurement scheduling are owned by Zephyr.
#pragma once
#include "Observed.h"
#include <math.h>
#include <stdint.h>

namespace mt::stock::telemetry::mlx90632 {
struct Calibration {
  double ea, eb, fa, fb, ga, gb, ka, ha, hb;
  zt::Observed<float> convert(int16_t six, int16_t nine, int16_t lower,
                              int16_t upper, float previous) const {
    const double reference = six / 12.0;
    const double vrTa = nine + gb * reference;
    const double vrTo = nine + ka * reference;
    if (!vrTa || !vrTo || !ea)
      return {};
    // Upstream stores AMB and Sto in float before its double calculation.
    const float amb = reference / vrTa * 524288.0;
    const float s = (lower + upper) / 2.0f;
    const float sto = (s / 12.0) / vrTo * 524288.0;
    const double ta = (amb - eb) / ea + 25.0;
    const double denominator =
        fa * ha * (1.0 + ga * (25.0 - previous) + fb * (ta - 25.0));
    if (!denominator)
      return {};
    // The stock loop uses the same previous result in all three iterations.
    const double argument = sto / denominator + pow(ta + 273.15, 4.0);
    if (!(argument >= 0.0))
      return {};
    const float object = pow(argument, 0.25) - 273.15 - hb;
    return isfinite(object) ? zt::Observed<float>(object)
                                 : zt::Observed<float>{};
  }
};

template <class Bus> class Sensor {
  Bus &bus_;
  Calibration calibration_{};
  enum class State { Cold, Ready, Converting };
  State state_ = State::Cold;
  bool touched_ = false, polled_ = false;
  uint64_t lastPoll_ = 0, started_ = 0, measured_ = 0;
  float previous_ = 0;
  zt::Observed<float> result_;
  bool setBits(uint16_t reg, uint16_t bits, uint16_t mask) {
    uint16_t old;
    return bus_.word(reg, old) && bus_.set(reg, (old & ~mask) | bits);
  }
  bool mode(unsigned value) { return setBits(0x3001, value << 1, 6); }
  void reset() {
    state_ = State::Cold;
    result_ = {};
    previous_ = 0;
  }
  bool identified() {
    uint16_t id;
    return bus_.word(0x24d5, id) && id == (bus_.address() >> 1);
  }
  bool initialize() {
    uint16_t status;
    if (!identified() ||
        !bus_.word(0x3fff, status) || (status & 0x0200))
      return false;
    touched_ = true;
    if (!mode(1))
      return false;
    const auto wide = [&](uint16_t offset, double scale, double &value) {
      int32_t raw;
      if (!bus_.wide(offset, raw))
        return false;
      value = raw * scale;
      return true;
    };
    const auto narrow = [&](uint16_t offset, double scale, double &value) {
      uint16_t bits;
      if (!bus_.word(offset, bits))
        return false;
      value = static_cast<int16_t>(bits) * scale;
      return true;
    };
    auto &c = calibration_;
    return wide(0x2424, 0x1p-16, c.ea) && wide(0x2426, 0x1p-8, c.eb) &&
           wide(0x2428, 0x1p-46, c.fa) && wide(0x242a, 0x1p-36, c.fb) &&
           wide(0x242c, 0x1p-36, c.ga) && narrow(0x242e, 0x1p-10, c.gb) &&
           narrow(0x242f, 0x1p-10, c.ka) && narrow(0x2481, 0x1p-14, c.ha) &&
           narrow(0x2482, 0x1p-14, c.hb) && c.ea && c.fa && c.ha && mode(2);
  }
  bool start(uint64_t now) {
    if (!setBits(0x3001, 8, 8) || !setBits(0x3fff, 0, 1))
      return false;
    state_ = State::Converting;
    started_ = now;
    return true;
  }
  void finish(uint64_t now) {
    uint16_t status;
    if (!bus_.word(0x3fff, status)) {
      reset();
      return;
    }
    if (!(status & 1)) {
      if (now - started_ >= 1500)
        reset();
      return;
    }
    const unsigned phase = (status >> 2) & 31;
    uint16_t six, nine, lower, upper;
    const uint16_t base = phase == 1 ? 0x4003 : 0x4006;
    if ((phase != 1 && phase != 2) || !bus_.word(0x4005, six) ||
        !bus_.word(0x4008, nine) || !bus_.word(base, lower) ||
        !bus_.word(base + 1, upper) || !setBits(0x3fff, 0, 1)) {
      reset();
      return;
    }
    result_ = calibration_.convert(
        static_cast<int16_t>(six), static_cast<int16_t>(nine),
        static_cast<int16_t>(lower), static_cast<int16_t>(upper), previous_);
    if (result_) {
      previous_ = *result_;
      measured_ = now;
    }
    state_ = State::Ready;
  }

public:
  explicit Sensor(Bus &bus) : bus_(bus) {}
  void poll(bool enabled, uint64_t now) {
    if (!enabled) {
      result_ = {};
      if (touched_ && (!polled_ || now < lastPoll_ || now - lastPoll_ >= 250)) {
        lastPoll_ = now;
        polled_ = true;
        if (identified() && mode(1))
          touched_ = false;
      }
      reset();
      return;
    }
    if (polled_ && now < lastPoll_)
      reset();
    else if (polled_ &&
             now - lastPoll_ < (state_ == State::Converting ? 100u : 1000u))
      return;
    polled_ = true;
    lastPoll_ = now;
    if (state_ == State::Converting) {
      finish(now);
      return;
    }
    if (state_ == State::Cold && !initialize()) {
      reset();
      return;
    }
    if (!start(now))
      reset();
  }
  zt::Observed<float> snapshot(uint64_t now) const {
    return now >= measured_ && now - measured_ <= 90000 ? result_
                                                        : zt::Observed<float>{};
  }
};
void poll(bool enabled, uint64_t now);
zt::Observed<float> snapshot(uint64_t now);
} // namespace mt::stock::telemetry::mlx90632
