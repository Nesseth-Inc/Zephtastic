// SPDX-License-Identifier: GPL-3.0-only
#include "GpioModuleBackend.h"

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

namespace mt::modules {
namespace {

#if DT_HAS_COMPAT_STATUS_OKAY(zephcore_gpio_module_exports)

static_assert(DT_NUM_INST_STATUS_OKAY(zephcore_gpio_module_exports) == 1,
              "GPIO module exports require at most one enabled dedicated node");

struct Export {
  gpio_dt_spec gpio{};
  uint32_t wirePin=0;
  bool readable=false;
  bool writable=false;
  bool pullup=false;
};

#define MT_GPIO_EXPORT(child)                                                   \
  Export{GPIO_DT_SPEC_GET(child, gpios), uint32_t(DT_REG_ADDR(child)),          \
         bool(DT_PROP_OR(child, zephcore_readable, 0)),                         \
         bool(DT_PROP_OR(child, zephcore_writable, 0)),                         \
         bool(DT_PROP_OR(child, zephcore_pullup, 0))},

// The trailing sentinel keeps empty child lists valid standard C++ arrays.
static const Export exports[]={
    DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zephcore_gpio_module_exports),
                                 MT_GPIO_EXPORT)
    Export{}};
#undef MT_GPIO_EXPORT
static constexpr size_t ExportCount=(sizeof(exports)/sizeof(exports[0]))-1;

class ZephyrGpioBackend final : public GpioModuleBackend {
  mutable GpioCapabilities caps_{};
  mutable bool initializationAttempted_=false;
  mutable bool tableValid_=true;
  mutable bool initializedReady_=false;
  mutable bool readinessLost_=false;

  bool allDevicesReady()const {
    for(size_t i=0;i<ExportCount;++i)
      if(!gpio_is_ready_dt(&exports[i].gpio))return false;
    return true;
  }

  bool validateTable()const {
    GpioCapabilities candidate{};
    for(size_t i=0;i<ExportCount;++i){
      const Export &entry=exports[i];
      if(entry.wirePin>=64||(!entry.readable&&!entry.writable)||
         (entry.pullup&&!entry.readable)||!entry.gpio.port)return false;
      const uint64_t bit=uint64_t(1)<<entry.wirePin;
      if(candidate.exported&bit)return false;
      candidate.exported|=bit;
      if(entry.readable)candidate.readable|=bit;
      if(entry.writable)candidate.writable|=bit;
      if(entry.pullup)candidate.pullup|=bit;
      for(size_t j=0;j<i;++j){
        const Export &prior=exports[j];
        if(prior.gpio.port==entry.gpio.port&&prior.gpio.pin==entry.gpio.pin)
          return false;
      }
    }
    return true;
  }

  int availability()const {
    initialize();
    if(!tableValid_)return -EINVAL;
    if(!initializedReady_||readinessLost_)return -ENODEV;
    if(!allDevicesReady()){
      readinessLost_=true;
      return -ENODEV;
    }
    return 0;
  }

  const Export *find(uint32_t wirePin)const {
    for(size_t i=0;i<ExportCount;++i)
      if(exports[i].wirePin==wirePin)return &exports[i];
    return nullptr;
  }

  int validateMask(uint64_t mask,uint64_t allowed,bool pullup)const {
    if(!mask)return -EINVAL;
    if(mask&~caps_.exported)return -EINVAL;
    if(mask&~allowed)return -EACCES;
    if(pullup&&(mask&~caps_.pullup))return -ENOTSUP;
    return 0;
  }

  // First operational access runs on the application's main context, after
  // Zephyr device initialization. Merely obtaining the backend reference from
  // another static object's constructor must not inspect device readiness.
  void initialize()const {
    if(initializationAttempted_)return;
    initializationAttempted_=true;
    tableValid_=validateTable();
    if(!tableValid_)return;
    if(!allDevicesReady()){
      readinessLost_=true;
      return;
    }
    initializedReady_=true;
    for(size_t i=0;i<ExportCount;++i){
      const Export &entry=exports[i];
      const uint64_t bit=uint64_t(1)<<entry.wirePin;
      caps_.exported|=bit;
      if(entry.readable)caps_.readable|=bit;
      if(entry.writable)caps_.writable|=bit;
      if(entry.pullup)caps_.pullup|=bit;
    }
  }

public:
  GpioCapabilities capabilities()const override {
    if(availability())return {};
    return caps_;
  }

  int configureInput(uint32_t wirePin,bool pullup)override {
    const int ready=availability();if(ready)return ready;
    if(wirePin>=64)return -EINVAL;
    const uint64_t bit=uint64_t(1)<<wirePin;
    const int permitted=validateMask(bit,caps_.readable,pullup);
    if(permitted)return permitted;
    const Export *entry=find(wirePin);if(!entry)return -EINVAL;
    const gpio_flags_t flags=GPIO_INPUT|(pullup?GPIO_PULL_UP:0);
    return gpio_pin_configure_dt(&entry->gpio,flags);
  }

  int read(uint64_t mask,bool pullup,uint64_t &value)override {
    const int ready=availability();if(ready)return ready;
    const int permitted=validateMask(mask,caps_.readable,pullup);
    if(permitted)return permitted;

    // Complete all direction/pull-up configuration before sampling any line.
    // A later driver failure may leave earlier pins configured, but the caller's
    // output value remains unchanged unless every requested read succeeds.
    for(size_t i=0;i<ExportCount;++i){
      const Export &entry=exports[i];
      if(!(mask&(uint64_t(1)<<entry.wirePin)))continue;
      const gpio_flags_t flags=GPIO_INPUT|(pullup?GPIO_PULL_UP:0);
      const int result=gpio_pin_configure_dt(&entry.gpio,flags);
      if(result)return result;
    }

    uint64_t candidate=0;
    for(size_t i=0;i<ExportCount;++i){
      const Export &entry=exports[i];
      const uint64_t bit=uint64_t(1)<<entry.wirePin;
      if(!(mask&bit))continue;
      const int result=gpio_pin_get_dt(&entry.gpio);
      if(result<0)return result;
      if(result>1)return -EIO;
      if(result)candidate|=bit;
    }
    value=candidate;
    return 0;
  }

  int write(uint64_t mask,uint64_t value)override {
    const int ready=availability();if(ready)return ready;
    if(value&~mask)return -EINVAL;
    const int permitted=validateMask(mask,caps_.writable,false);
    if(permitted)return permitted;

    // Per-pin GPIO drivers cannot guarantee an atomic multi-line transition.
    // Validate the complete mask above before the first physical operation.
    for(size_t i=0;i<ExportCount;++i){
      const Export &entry=exports[i];
      const uint64_t bit=uint64_t(1)<<entry.wirePin;
      if(!(mask&bit))continue;
      const gpio_flags_t initial=(value&bit)?GPIO_OUTPUT_ACTIVE:GPIO_OUTPUT_INACTIVE;
      const int result=gpio_pin_configure_dt(&entry.gpio,initial);
      if(result)return result;
      const int written=gpio_pin_set_dt(&entry.gpio,(value&bit)!=0);
      if(written)return written;
    }
    return 0;
  }
};

#else

class ZephyrGpioBackend final : public GpioModuleBackend {
public:
  GpioCapabilities capabilities()const override{return {};}
  int configureInput(uint32_t,bool)override{return -ENODEV;}
  int read(uint64_t,bool,uint64_t &)override{return -ENODEV;}
  int write(uint64_t,uint64_t)override{return -ENODEV;}
};

#endif

// Static storage avoids function-local C++ guard-runtime dependencies in the
// minimal target toolchain. Construction performs no GPIO/device operations.
ZephyrGpioBackend backend;

} // namespace

GpioModuleBackend &zephyrGpioModuleBackend(){
  return backend;
}

} // namespace mt::modules
