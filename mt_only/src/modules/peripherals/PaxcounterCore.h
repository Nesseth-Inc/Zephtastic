// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../../stock/peripherals/PaxcounterModule.h"
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
#include "../../stock/peripherals/PaxInterval.h"
#endif
#include <stddef.h>
#include <stdint.h>
#include "meshtastic/module_config.pb.h"
#include "meshtastic/paxcount.pb.h"

namespace mt::modules::peripherals {

// Fixed-capacity unique-observation window. Scanner/companion code owns radio
// setup and calls observe only for passive reports it already receives.
class PaxcounterCore {
public:
  static constexpr size_t Capacity = 128;
  static constexpr uint32_t DefaultIntervalSeconds = 900;
  static constexpr int32_t DefaultThresholdDbm = -80;
  static constexpr uint32_t WindowMs = 60000;
  struct Config {
    bool enabled = false;
    uint32_t updateIntervalSeconds = 0;
    int32_t wifiThreshold = 0;
    int32_t bleThreshold = 0;
  };
  enum class Kind : uint8_t { Wifi, Ble };
  enum class ObserveResult : uint8_t { Disabled, WeakSignal, Duplicate, Recorded, Full };
  enum class PollResult : uint8_t { Disabled, NotDue, NoFreshData, Ready, NotSubmitted };

  void configure(const Config &config, uint32_t nowMs) {
    config_ = config;
    if (!config_.updateIntervalSeconds) config_.updateIntervalSeconds = DefaultIntervalSeconds;
    if (config_.updateIntervalSeconds > UINT32_MAX / 1000u)
      config_.updateIntervalSeconds = DefaultIntervalSeconds;
    if (!config_.wifiThreshold) config_.wifiThreshold = DefaultThresholdDbm;
    if (!config_.bleThreshold) config_.bleThreshold = DefaultThresholdDbm;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    interval_.reset(nowMs);reportedDataSent_=true;
#else
    clear();
#endif
    lastReportMs_ = nowMs;
    configured_ = true;
  }

  ObserveResult observe(Kind kind, const uint8_t address[6], int8_t rssi, uint32_t nowMs) {
    if (!configured_ || !config_.enabled || !address) return ObserveResult::Disabled;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    uint16_t id=0;
    if(!stock::paxcounter::Interval::bucket(address,false,id))return ObserveResult::Disabled;
    return observeBucket(kind,id,rssi,nowMs);
#else
    if (rssi < (kind == Kind::Wifi ? config_.wifiThreshold : config_.bleThreshold))
      return ObserveResult::WeakSignal;
    expire(nowMs);
    for (size_t i=0;i<Capacity;++i) {
      Entry &entry = entries_[i];
      if (entry.used && entry.kind == kind && same(entry.address, address))
        return ObserveResult::Duplicate;
    }
    for (size_t i=0;i<Capacity;++i) {
      Entry &entry = entries_[i];
      if (!entry.used) {
        entry.used = true; entry.kind = kind; entry.seenMs = nowMs;
        for (size_t j=0;j<6;++j) entry.address[j]=address[j];
        if (kind == Kind::Wifi) ++wifi_; else ++ble_;
        reportedDataSent_ = false;
        return ObserveResult::Recorded;
      }
    }
    return ObserveResult::Full;
#endif
  }
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  void publishIntervals(uint32_t now){if(enabled())advance(now);}
  ObserveResult observeBucket(Kind kind,uint16_t id,int8_t rssi,uint32_t now) {
    if(!enabled())return ObserveResult::Disabled;
    // Pinned libpax blescan.cpp converts the raw HCI byte with -(0xff-byte).
    const int signal=kind==Kind::Ble?int(uint8_t(rssi))-255:int(rssi);
    if(signal<(kind==Kind::Wifi?config_.wifiThreshold:config_.bleThreshold))return ObserveResult::WeakSignal;
    if(int32_t(now-interval_.start)<0)return ObserveResult::Disabled;
    advance(now);
    return interval_.add(id,kind==Kind::Wifi)?ObserveResult::Recorded:ObserveResult::Duplicate;
  }
#endif

  // Meshtastic 8e6a88d0, src/modules/esp32/PaxcounterModule.cpp::sendInfo.
  // The owner submits the protobuf through Service. A refused submission leaves
  // the report pending for the next interval, as a failed stock allocation does.
  template<typename Submit>
  PollResult sendInfo(uint32_t nowMs, uint32_t uptimeSeconds, Submit submit) {
    if (!configured_ || !config_.enabled) return PollResult::Disabled;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    (void)nowMs;
    if(reportedDataSent_||lastReportMs_==interval_.start)return PollResult::NotDue;
    lastReportMs_=interval_.start;
    return stock::paxcounter::sendInfo(reportedDataSent_,interval_.reportedWifi,interval_.reportedBle,uptimeSeconds,submit)
      ? PollResult::Ready : PollResult::NotSubmitted;
#else
    expire(nowMs);
    const uint32_t interval = config_.updateIntervalSeconds * 1000u;
    if (static_cast<uint32_t>(nowMs - lastReportMs_) < interval) return PollResult::NotDue;
    lastReportMs_ = nowMs;
    if (reportedDataSent_) return PollResult::NoFreshData;

    return stock::paxcounter::sendInfo(reportedDataSent_, wifi_, ble_, uptimeSeconds, submit)
        ? PollResult::Ready : PollResult::NotSubmitted;
#endif
  }

  PollResult poll(uint32_t nowMs, uint32_t uptimeSeconds, meshtastic_Paxcount &out) {
    return sendInfo(nowMs, uptimeSeconds, [&](const meshtastic_Paxcount &pl) {
      out = pl;
      return true;
    });
  }

  // An active provider returns a valid zero count even before the first report.
  bool reply(uint32_t nowMs, uint32_t uptimeSeconds, meshtastic_Paxcount &out) {
    if (!enabled()) return false;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    (void)nowMs;
    out=stock::paxcounter::allocReply(interval_.reportedWifi,interval_.reportedBle,uptimeSeconds);
#else
    expire(nowMs);
    out=stock::paxcounter::allocReply(wifi_,ble_,uptimeSeconds);
#endif
    return true;
  }

  bool enabled() const { return configured_ && config_.enabled; }
  Config config() const { return config_; }
#if !defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  uint32_t wifiCount() const { return wifi_; }
  uint32_t bleCount() const { return ble_; }

private:
  struct Entry { uint8_t address[6]{}; uint32_t seenMs=0; Kind kind=Kind::Wifi; bool used=false; };
  Entry entries_[Capacity]{};
  uint32_t wifi_=0, ble_=0;
  static bool same(const uint8_t a[6], const uint8_t b[6]) {
    uint8_t different=0; for(size_t i=0;i<6;++i) different|=a[i]^b[i]; return different==0;
  }
  void clear() { for (auto &entry : entries_) entry=Entry{}; wifi_=ble_=0; reportedDataSent_=true; }
  void expire(uint32_t nowMs) {
    for (auto &entry : entries_) if (entry.used && static_cast<uint32_t>(nowMs-entry.seenMs)>=WindowMs) {
      if(entry.kind==Kind::Wifi) --wifi_; else --ble_;
      entry.used=false;
    }
  }
#else
private:
  stock::paxcounter::Interval interval_;
  void advance(uint32_t now){if(interval_.advance(now,config_.updateIntervalSeconds*1000u))reportedDataSent_=false;}
#endif
  Config config_{};
  uint32_t lastReportMs_=0;
  bool configured_=false,reportedDataSent_=true;
};
} // namespace mt::modules::peripherals
