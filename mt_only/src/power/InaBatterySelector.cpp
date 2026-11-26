// SPDX-License-Identifier: GPL-3.0-only
#include "InaBatterySelector.h"

#include <math.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>

namespace mt::power::ina {
namespace {
struct Candidate {
  const struct device *device;
  uint32_t address;
};

// Deliberately enumerate only known INA node labels and use each node's actual
// reg property. No bus scan, guessed address, or generic sensor enumeration.
constexpr Candidate candidates[] = {
#if DT_NODE_EXISTS(DT_NODELABEL(ina219)) && DT_NODE_HAS_STATUS(DT_NODELABEL(ina219), okay)
    { DEVICE_DT_GET(DT_NODELABEL(ina219)), DT_REG_ADDR(DT_NODELABEL(ina219)) },
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(ina226)) && DT_NODE_HAS_STATUS(DT_NODELABEL(ina226), okay)
    { DEVICE_DT_GET(DT_NODELABEL(ina226)), DT_REG_ADDR(DT_NODELABEL(ina226)) },
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(ina260)) && DT_NODE_HAS_STATUS(DT_NODELABEL(ina260), okay)
    { DEVICE_DT_GET(DT_NODELABEL(ina260)), DT_REG_ADDR(DT_NODELABEL(ina260)) },
#endif
#if defined(CONFIG_MT_INA3221_BAT_CHANNEL)
    { DEVICE_DT_GET(DT_NODELABEL(ina3221)), DT_REG_ADDR(DT_NODELABEL(ina3221)) },
#endif
    { nullptr, 0 },
};

constexpr uint64_t SampleIntervalMs=1000;
constexpr uint64_t FreshnessLimitMs=5000;
const struct device *selected=nullptr;
Snapshot latest{};
uint64_t lastAttemptMs=0;
bool attempted=false;

const Candidate *findCandidate(uint32_t address) {
  if(!address)return nullptr;
  const Candidate *match=nullptr;
  for(const auto &candidate:candidates) {
    if(!candidate.device||candidate.address!=address)continue;
    // If the devicetree describes multiple candidate parts at one address,
    // refuse the ambiguous selection rather than choosing by source order.
    if(match)return nullptr;
    match=&candidate;
  }
  return match;
}

void clearReading(Status status) {
  latest.status=status;
  latest.busVoltageV=0.0f;
  latest.currentA=0.0f;
  latest.sampledAtMs=0;
}

bool readFloat(const struct device *device,enum sensor_channel channel,float &out) {
  struct sensor_value value{};
  if(sensor_channel_get(device,channel,&value)!=0)return false;
  const float converted=sensor_value_to_float(&value);
  if(!isfinite(converted))return false;
  out=converted;
  return true;
}
} // namespace

bool supports(uint32_t address) {
  return address==0||findCandidate(address)!=nullptr;
}

bool configure(uint32_t address) {
  if(address==0) {
    selected=nullptr;
    latest={};
    latest.status=Status::Disabled;
    attempted=false;
    lastAttemptMs=0;
    return true;
  }
  const Candidate *candidate=findCandidate(address);
  if(!candidate) {
    selected=nullptr;
    latest={};
    latest.address=address;
    clearReading(Status::UnsupportedAddress);
    attempted=false;
    return false;
  }

  selected=candidate->device;
  latest={};
  latest.address=address;
  attempted=false;
  lastAttemptMs=0;
  // ZephCore's environment adapter may already have initialized this same
  // deferred node. Never call device_init() for a ready, already-owned device.
  if(!device_is_ready(selected)) {
    (void)device_init(selected);
  }
  if(!device_is_ready(selected)) {
    clearReading(Status::NotReady);
    return false;
  }
  clearReading(Status::NoSample);
  return true;
}

void poll(uint64_t nowMs) {
  if(!selected)return;
  if(nowMs>INT64_MAX) {clearReading(Status::ReadError);return;}
  if(attempted&&nowMs>=lastAttemptMs&&nowMs-lastAttemptMs<SampleIntervalMs)return;
  if(attempted&&nowMs<lastAttemptMs) {
    clearReading(Status::ReadError);
    lastAttemptMs=nowMs;
    return;
  }
  attempted=true;
  lastAttemptMs=nowMs;
#if defined(CONFIG_MT_INA3221_BAT_CHANNEL)
  if(selected==DEVICE_DT_GET(DT_NODELABEL(ina3221))){
    // Environment/Power polling also selects this device's active channel.
    // Set it afresh for each battery read on the shared main-thread owner.
    sensor_value channel{CONFIG_MT_INA3221_BAT_CHANNEL,0};
    if(sensor_attr_set(selected,SENSOR_CHAN_ALL,static_cast<sensor_attribute>(SENSOR_ATTR_PRIV_START+1),&channel)){
      clearReading(Status::ReadError);return;
    }
  }
#endif
  if(!device_is_ready(selected)||sensor_sample_fetch(selected)!=0) {
    clearReading(Status::ReadError);
    return;
  }
  float voltage=0.0f,current=0.0f;
  if(!readFloat(selected,SENSOR_CHAN_VOLTAGE,voltage)||
     !readFloat(selected,SENSOR_CHAN_CURRENT,current)) {
    clearReading(Status::ReadError);
    return;
  }
  latest.status=Status::Fresh;
  latest.busVoltageV=voltage;
  latest.currentA=current;
  latest.sampledAtMs=nowMs;
}

Snapshot snapshot(uint64_t nowMs) {
  Snapshot result=latest;
  if(result.status==Status::Fresh&&
     (nowMs<result.sampledAtMs||nowMs-result.sampledAtMs>FreshnessLimitMs)) {
    result.status=Status::Stale;
    result.busVoltageV=0.0f;
    result.currentA=0.0f;
  }
  return result;
}

} // namespace mt::power::ina
