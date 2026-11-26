// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "HardwareSnapshot.h"
#include "power/PowerReadings.h"

namespace mt::power::max17048 {
struct Reading {
  float volts = 0;
  uint8_t percent = 0;
  bool present = false;
  uint16_t rawSoc = 0;
  bool positiveRate = false, charging = false;

  void device(HardwareSnapshot &out) const {
    if (!present || volts < 2 || volts > 4.5f) return;
    out.cellMilliVolts = static_cast<uint16_t>(volts * 1000.0f);
    out.cellPercent = percent;
    out.gaugeCharging = charging;
    if (volts >= 4.195f) out.externalPower = true;
  }
  void power(zt::telemetry::PowerReadings &out) const {
    if (present) out.voltage[0] = volts;
  }
};

// Upstream MAX17048Sensor and Adafruit_MAX1704X 1.0.3 use the fractional SOC
// register before rounding, and 78.125 microvolts per VCELL unit.
template<class Bus> Reading measure(Bus &bus) {
  uint16_t version, mode, config, voltage, soc, rate;
  if (!bus.word(0x08, version) || (version & 0xfff0) != 0x0010 ||
      !bus.word(0x06, mode) || !bus.word(0x0c, config)) return {};
  // An accessory may have been left asleep by another firmware. Only clear
  // the two sleep controls and allow a full sample interval after waking.
  if ((mode & 0x2000) || (config & 0x0080)) {
    if ((config & 0x0080) && !bus.set(0x0c, config & ~0x0080)) return {};
    if (mode & 0x2000) (void)bus.set(0x06, mode & ~0x2000);
    return {};
  }
  if (!bus.word(0x02, voltage) || !bus.word(0x04, soc) || !bus.word(0x16, rate)) return {};
  const uint16_t rounded = (static_cast<uint32_t>(soc) + 128) / 256;
  return {static_cast<float>(voltage * 78.125 / 1000000.0),
          static_cast<uint8_t>(rounded > 100 ? 100 : rounded), true, soc,
          // Signed CRATE is 0.208 %/h/count. Five is the first count greater than 1 %/h.
          rate>=5 && rate<=0x7fff, false};
}

class Sample {
 public:
  template<class Bus> void poll(Bus &bus, uint64_t now) {
    if (sampled_ && now >= last_ && now - last_ < 1000) return;
    const bool backwards=sampled_ && now<last_;
    last_ = now;
    sampled_ = true;
    latest_ = measure(bus);
    if (backwards || !latest_.present) {count_=0; next_=0; charging_=false;}
    if (!latest_.present) return;
    identified_=true;
    // Power::runOnce samples charging every 20 s after initialization.
    // Keep the unrounded SOC register: a sub-percent rise can indicate charging.
    if (!count_ || now-trendAt_>=20000) {
      trendAt_=now;
      history_[next_]={latest_.rawSoc,latest_.positiveRate};
      next_=(next_+1)%3;
      if(count_<3)++count_;
      const auto &oldest=history_[count_==3?next_:0];
      charging_=oldest.positiveRate && latest_.positiveRate && oldest.soc<latest_.rawSoc;
    }
    latest_.charging=charging_;
  }
  bool identified()const{return identified_;}
  Reading get(uint64_t now) const {
    return sampled_ && now >= last_ && now - last_ < 5000 ? latest_ : Reading{};
  }
 private:
  Reading latest_;
  uint64_t last_ = 0;
  bool sampled_ = false, identified_=false, charging_=false;
  struct TrendPoint {uint16_t soc;bool positiveRate;};
  TrendPoint history_[3]{};
  uint8_t count_=0,next_=0;
  uint64_t trendAt_=0;
};
void poll(uint64_t now);
bool identified();
Reading snapshot(uint64_t now);
}
