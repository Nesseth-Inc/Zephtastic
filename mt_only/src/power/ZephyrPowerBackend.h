// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PowerPolicy.h"
#include <zephyr/kernel.h>

namespace mt::power {
// Owner hooks must close new work, drain/stop peripherals safely, and validate
// that queues and bus transactions are quiescent. Install hooks from the main
// owner and execute callbacks only after releasing Service/Companion locks.
struct PowerOwnerHooks {
  void *context=nullptr;
  bool (*prepare)(void *)=nullptr;
  bool (*quiescent)(void *)=nullptr;
  void (*resume)(void *)=nullptr;
  bool (*preparePowerOff)(void *)=nullptr;
  bool (*quiescentPowerOff)(void *)=nullptr;
  void (*resumePowerOff)(void *)=nullptr;
};

// Reusable Zephyr timed-idle backend. It blocks only the owner thread; the
// kernel timer and any ISR that calls notifyActivity() wake it. The platform
// idle path may enter a low-power state when CONFIG_PM permits it. It does not
// shut down the radio, buses, BLE, or storage on its own.
class ZephyrPowerBackend {
  k_sem wake_{};
  PowerOwnerHooks owner_{};
  bool initialized_=false,prepared_=false;
  static bool timedWakePath(void *,uint32_t);
  static bool prepareTimed(void *);
  static bool timedQuiescent(void *);
  static int enterTimed(void *,uint32_t);
  static void resumeTimed(void *);
  static bool preparePowerOff(void *);
  static bool powerOffQuiescent(void *);
  static void resumePowerOff(void *);
#if defined(CONFIG_BOARD_HELTEC_T096) && defined(CONFIG_POWEROFF) && defined(CONFIG_SOC_SERIES_NRF52)
  static bool manualWakeReady(void *);
  static int enterManualPoweroff(void *);
#endif
#if defined(CONFIG_BOARD_XIAO_ESP32S3) && defined(CONFIG_POWEROFF) && defined(CONFIG_SOC_SERIES_ESP32S3)
  static bool xiaoManualWakeReady(void *);
  static int enterXiaoManualPoweroff(void *);
#endif
public:
  static ZephyrPowerBackend &instance();
  // Safe for ISR use even when CompletionRuntime itself lives in external RAM.
  static void notifyGlobalActivity();
  ZephyrPowerBackend()=default;
  ZephyrPowerBackend(const ZephyrPowerBackend &)=delete;
  ZephyrPowerBackend &operator=(const ZephyrPowerBackend &)=delete;
  bool configure(const PowerOwnerHooks &owner);
  void notifyActivity(); // ISR-safe wake signal for radio/button/BLE ingress.
  PowerPolicy::DeepSleepBackend timedBackend();
  PowerPolicy::PowerOffBackend manualPowerOffBackend();
  static bool timedSleepAvailable();
  static bool manualPowerOffAvailable();
};
}
