// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrEntropy.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/irq.h>
#include <errno.h>
#include <string.h>
#include "monocypher.h"

#if defined(CONFIG_SOC_NRF52840)
#include <hal/nrf_rng.h>
#elif defined(CONFIG_SOC_SERIES_ESP32S3)
#include <esp_bt.h>
#include <esp_cpu.h>
#include <esp_private/phy.h>
#include <esp_private/esp_clk.h>
#include <hal/rng_ll.h>
#endif

namespace mt::pki {
namespace {
K_SEM_DEFINE(entropyLease,1,1);
const struct device *device(){return DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_entropy));}
}
uint32_t ZephyrEntropyPort::micros()const{return uint32_t(k_ticks_to_us_floor64(k_uptime_ticks()));}
bool ZephyrEntropyPort::acquire() {
  if(k_is_in_isr()||held_||k_sem_take(&entropyLease,K_NO_WAIT))return false;
  held_=true;
#if defined(CONFIG_SOC_SERIES_ESP32S3) && defined(CONFIG_BT) && !defined(CONFIG_SMP) && !defined(CONFIG_PM)
  // Pinned HAL's sleep and teardown paths both take this mutex before changing
  // PHY supply. No enable/wakeup API is called. Private HAL API: re-review on update.
  auto *lock=phy_get_lock();
  if(!lock||k_mutex_lock(lock,K_NO_WAIT)){held_=false;k_sem_give(&entropyLease);return false;}
  phyLock_=lock;cpuHz_=esp_clk_cpu_freq();apbHz_=esp_clk_apb_freq();
#endif
  if(!ready()){release();return false;}
  return true;
}
bool ZephyrEntropyPort::ready()const {
  if(!held_||!device()||!device_is_ready(device()))return false;
#if defined(CONFIG_SOC_NRF52840) && defined(CONFIG_ENTROPY_NRF5_RNG) && defined(CONFIG_ENTROPY_NRF5_BIAS_CORRECTION)
  return DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_entropy),nordic_nrf_rng)&&
    (NRF_RNG->CONFIG&RNG_CONFIG_DERCEN_Msk)!=0;
#elif defined(CONFIG_SOC_SERIES_ESP32S3) && defined(CONFIG_BT) && !defined(CONFIG_SMP) && !defined(CONFIG_PM)
  return DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_entropy),espressif_esp32_trng)&&
    phyLock_&&apbHz_==80000000u&&(cpuHz_==80000000u||cpuHz_==160000000u||cpuHz_==240000000u)&&
    uint32_t(esp_clk_cpu_freq())==cpuHz_&&uint32_t(esp_clk_apb_freq())==apbHz_&&
    esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED&&
    (phy_get_modem_flag()&PHY_MODEM_BT);
#else
  return false;
#endif
}
int ZephyrEntropyPort::read(uint8_t *out,size_t capacity) {
  if(!out||!capacity||capacity>BoundedEntropy::Capacity||!ready())return -EIO;
#if defined(CONFIG_SOC_NRF52840) && defined(CONFIG_ENTROPY_NRF5_RNG) && defined(CONFIG_ENTROPY_NRF5_BIAS_CORRECTION)
  // flags=0 selects ONLY the finite pool-copy branch. Never thread get_entropy,
  // ENTROPY_BUSYWAIT, direct RNG ownership or an ENOSYS fallback.
  return entropy_get_entropy_isr(device(),out,uint16_t(capacity),0);
#elif defined(CONFIG_SOC_SERIES_ESP32S3) && defined(CONFIG_BT) && !defined(CONFIG_SMP) && !defined(CONFIG_PM)
  // A full 1778 APB cycles precedes every word, including the first. The pinned
  // driver sets this S3 limit. IRQ exclusion prevents another local RNG reader
  // from draining the source between spacing and read (SMP/PM refused above).
  constexpr unsigned MaxCyclePolls=16384;
  const uint32_t spacing=(cpuHz_/apbHz_)*1778u;
  const unsigned key=irq_lock();
  const uint32_t start=esp_cpu_get_cycle_count();
  bool elapsed=false;
  for(unsigned i=0;i<MaxCyclePolls;++i) {
    if(uint32_t(esp_cpu_get_cycle_count()-start)>=spacing){elapsed=true;break;}
  }
  uint32_t word=0;
  if(elapsed&&ready())word=rng_ll_read_data();
  else elapsed=false;
  irq_unlock(key);
  if(!elapsed){crypto_wipe(&word,sizeof(word));return -EIO;}
  const size_t count=capacity<sizeof(word)?capacity:sizeof(word);
  memcpy(out,&word,count);crypto_wipe(&word,sizeof(word));return int(count);
#else
  return -ENOTSUP;
#endif
}
void ZephyrEntropyPort::pause() {
#if defined(CONFIG_SOC_NRF52840)
  // Bounded scheduler wait; no wait while holding IRQ or driver locks.
  if(held_)k_usleep(250);
#endif
}
bool ZephyrEntropyPort::release() {
  if(!held_)return false;
#if defined(CONFIG_SOC_SERIES_ESP32S3) && defined(CONFIG_BT) && !defined(CONFIG_SMP) && !defined(CONFIG_PM)
  if(phyLock_&&k_mutex_unlock(static_cast<struct k_mutex *>(phyLock_)))return false;
#endif
  phyLock_=nullptr;cpuHz_=apbHz_=0;held_=false;k_sem_give(&entropyLease);return true;
}
}
