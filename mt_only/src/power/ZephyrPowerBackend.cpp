// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrPowerBackend.h"
#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#if defined(CONFIG_BOARD_HELTEC_T096) && defined(CONFIG_POWEROFF) && defined(CONFIG_SOC_SERIES_NRF52) && DT_HAS_ALIAS(sw0)
#include <hal/nrf_gpio.h>
#include <zephyr/sys/poweroff.h>
#define MT_HAS_T096_MANUAL_POWEROFF 1
static_assert(DT_GPIO_PIN(DT_ALIAS(sw0),gpios)==10,"T096 wake button must remain P1.10");
static_assert(DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(sw0),gpios),DT_NODELABEL(gpio1)),"T096 wake button must remain on GPIO port 1");
static_assert((DT_GPIO_FLAGS(DT_ALIAS(sw0),gpios)&GPIO_ACTIVE_LOW)!=0,"T096 wake button must be active low");
static_assert((DT_GPIO_FLAGS(DT_ALIAS(sw0),gpios)&GPIO_PULL_UP)!=0,"T096 wake button must have pull-up");
static const struct gpio_dt_spec T096WakeButton=GPIO_DT_SPEC_GET(DT_ALIAS(sw0),gpios);
static bool manualWakeConfigured=false;
#else
#define MT_HAS_T096_MANUAL_POWEROFF 0
#endif

#if defined(CONFIG_BOARD_XIAO_ESP32S3) && defined(CONFIG_POWEROFF) && \
    defined(CONFIG_SOC_SERIES_ESP32S3) && DT_HAS_ALIAS(sw0)
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <soc/gpio_num.h>
#include <zephyr/sys/poweroff.h>
#define MT_HAS_XIAO_MANUAL_POWEROFF 1
static_assert(DT_GPIO_PIN(DT_ALIAS(sw0),gpios)==21,"XIAO wake button must remain GPIO0.21");
static_assert(DT_SAME_NODE(DT_GPIO_CTLR(DT_ALIAS(sw0),gpios),DT_NODELABEL(gpio0)),"XIAO wake button must remain on GPIO0");
static_assert((DT_GPIO_FLAGS(DT_ALIAS(sw0),gpios)&GPIO_ACTIVE_LOW)!=0,"XIAO wake button must be active low");
static_assert((DT_GPIO_FLAGS(DT_ALIAS(sw0),gpios)&GPIO_PULL_UP)!=0,"XIAO wake button must have pull-up");
static const struct gpio_dt_spec XiaoWakeButton=GPIO_DT_SPEC_GET(DT_ALIAS(sw0),gpios);
static bool xiaoWakeConfigured=false;
#else
#define MT_HAS_XIAO_MANUAL_POWEROFF 0
#endif

namespace mt::power {
namespace {
// The ISR-visible semaphore must reside in internal BSS. CompletionRuntime is
// optionally placed in ESP external RAM, which is inaccessible while flash
// cache is disabled during storage writes.
ZephyrPowerBackend globalPowerBackend;
}
ZephyrPowerBackend &ZephyrPowerBackend::instance(){return globalPowerBackend;}
void ZephyrPowerBackend::notifyGlobalActivity(){globalPowerBackend.notifyActivity();}
bool ZephyrPowerBackend::configure(const PowerOwnerHooks &owner){
  if(!owner.prepare||!owner.quiescent||!owner.resume)return false;
  owner_=owner;k_sem_init(&wake_,0,1);initialized_=true;prepared_=false;
#if MT_HAS_T096_MANUAL_POWEROFF
  manualWakeConfigured=device_is_ready(T096WakeButton.port)&&gpio_pin_configure_dt(&T096WakeButton,GPIO_INPUT)==0;
#endif
#if MT_HAS_XIAO_MANUAL_POWEROFF
  xiaoWakeConfigured=device_is_ready(XiaoWakeButton.port)&&gpio_pin_configure_dt(&XiaoWakeButton,GPIO_INPUT)==0;
#endif
  return true;
}
bool ZephyrPowerBackend::timedSleepAvailable(){return true;}
void ZephyrPowerBackend::notifyActivity(){if(initialized_)k_sem_give(&wake_);}
bool ZephyrPowerBackend::timedWakePath(void *context,uint32_t){
  auto *self=static_cast<ZephyrPowerBackend *>(context);return self&&self->initialized_&&self->owner_.prepare&&self->owner_.quiescent&&self->owner_.resume;
}
bool ZephyrPowerBackend::prepareTimed(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);if(!self||!self->initialized_)return false;
  if(!self->owner_.prepare(self->owner_.context)){
    self->owner_.resume(self->owner_.context);return false;
  }
  self->prepared_=true;return true;
}
bool ZephyrPowerBackend::timedQuiescent(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);
  return self&&self->initialized_&&self->prepared_&&
         self->owner_.quiescent(self->owner_.context);
}
int ZephyrPowerBackend::enterTimed(void *context,uint32_t duration){
  auto *self=static_cast<ZephyrPowerBackend *>(context);if(!self||!self->prepared_)return -EINVAL;
  // The application owner has an eight-second watchdog and other scheduled
  // work. A timer wake is a housekeeping continuation, not new user activity;
  // return at least once per second so the owner can service those deadlines.
  // RX remains enabled, and notifyActivity() still wakes this semaphore early.
  (void)duration;
  const k_timeout_t timeout=K_SECONDS(1);
  const int result=k_sem_take(&self->wake_,timeout);
  // 0 is an activity wake; 1 is an ordinary one-second timer wake. Other
  // failures are not reported as successful sleep.
  return result==0?0:(result==-EAGAIN?1:result);
}
void ZephyrPowerBackend::resumeTimed(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);if(!self||!self->prepared_)return;
  self->prepared_=false;self->owner_.resume(self->owner_.context);
}
bool ZephyrPowerBackend::preparePowerOff(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);
  if(!self||!self->initialized_||!self->owner_.preparePowerOff||!self->owner_.quiescentPowerOff||!self->owner_.resumePowerOff)return false;
  if(!self->owner_.preparePowerOff(self->owner_.context)){
    self->owner_.resumePowerOff(self->owner_.context);return false;
  }
  self->prepared_=true;return true;
}
bool ZephyrPowerBackend::powerOffQuiescent(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);
  return self&&self->initialized_&&self->prepared_&&
         self->owner_.quiescentPowerOff&&self->owner_.quiescentPowerOff(self->owner_.context);
}
void ZephyrPowerBackend::resumePowerOff(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);if(!self||!self->prepared_)return;
  self->prepared_=false;if(self->owner_.resumePowerOff)self->owner_.resumePowerOff(self->owner_.context);
}
PowerPolicy::DeepSleepBackend ZephyrPowerBackend::timedBackend(){
  return {this,&timedWakePath,&prepareTimed,&timedQuiescent,&enterTimed,&resumeTimed};
}
bool ZephyrPowerBackend::manualPowerOffAvailable(){
#if MT_HAS_T096_MANUAL_POWEROFF
  return manualWakeConfigured&&device_is_ready(T096WakeButton.port);
#elif MT_HAS_XIAO_MANUAL_POWEROFF
  return xiaoWakeConfigured&&device_is_ready(XiaoWakeButton.port);
#else
  return false;
#endif
}
#if MT_HAS_T096_MANUAL_POWEROFF
bool ZephyrPowerBackend::manualWakeReady(void *){
  // The GPIO is a valid wake source when released. A held key is checked again
  // at the final action boundary to avoid an immediate System OFF bounce.
  return manualWakeConfigured&&device_is_ready(T096WakeButton.port);
}
int ZephyrPowerBackend::enterManualPoweroff(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);
  if(!self||!self->prepared_||
     !self->owner_.quiescentPowerOff||!self->owner_.quiescentPowerOff(self->owner_.context)||
     gpio_pin_get_dt(&T096WakeButton)!=0)return -EAGAIN;
  // Nordic's GPIO wakeup-source property alone does not set SENSE in the
  // nRF52 driver. Configure the verified T096 active-low button directly.
  nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(1,10),NRF_GPIO_PIN_PULLUP,NRF_GPIO_PIN_SENSE_LOW);
  sys_poweroff();
  return -EIO; // A real poweroff must not return.
}
#endif
#if MT_HAS_XIAO_MANUAL_POWEROFF
bool ZephyrPowerBackend::xiaoManualWakeReady(void *){
  // GPIO21 is an ESP32-S3 RTC IO. Do not advertise the wake path unless the
  // declared board button is ready and released; a held button would cause an
  // immediate deep-sleep wake/reboot cycle.
  return xiaoWakeConfigured&&device_is_ready(XiaoWakeButton.port)&&
         gpio_pin_get_dt(&XiaoWakeButton)==0;
}
int ZephyrPowerBackend::enterXiaoManualPoweroff(void *context){
  auto *self=static_cast<ZephyrPowerBackend *>(context);
  if(!self||!self->prepared_||!self->owner_.quiescentPowerOff||
     !self->owner_.quiescentPowerOff(self->owner_.context)||
     !xiaoManualWakeReady(nullptr))return -EAGAIN;

  // Configure the already-declared active-low button as an RTC input and use
  // ESP32-S3 ext0 low-level wake. Zephyr's pinned sys_poweroff backend only
  // forces the RTC domain on, applies configured GPIO sleep holds, then calls
  // esp_deep_sleep_start(); it does not clear ext0 configuration.
  const auto restoreButton=[](){
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    (void)rtc_gpio_deinit(GPIO_NUM_21);
    return gpio_pin_configure_dt(&XiaoWakeButton,GPIO_INPUT);
  };
  if(rtc_gpio_init(GPIO_NUM_21)!=ESP_OK||
     rtc_gpio_set_direction(GPIO_NUM_21,RTC_GPIO_MODE_INPUT_ONLY)!=ESP_OK||
     rtc_gpio_pullup_en(GPIO_NUM_21)!=ESP_OK||
     rtc_gpio_pulldown_dis(GPIO_NUM_21)!=ESP_OK||
     esp_sleep_enable_ext0_wakeup(GPIO_NUM_21,0)!=ESP_OK){
    (void)restoreButton();return -EIO;
  }

  // Recheck at the final boundary after configuring RTC IO. This does not
  // clear the wake source and prevents a newly-held button from entering OFF.
  if(rtc_gpio_get_level(GPIO_NUM_21)!=1||!self->owner_.quiescentPowerOff(self->owner_.context)){
    const int restored=restoreButton();return restored?-EIO:-EAGAIN;
  }
  sys_poweroff();
  return -EIO; // A real poweroff must not return.
}
#endif
PowerPolicy::PowerOffBackend ZephyrPowerBackend::manualPowerOffBackend(){
#if MT_HAS_T096_MANUAL_POWEROFF
  if(!owner_.preparePowerOff||!owner_.quiescentPowerOff||!owner_.resumePowerOff)return {};
  return {this,&manualWakeReady,&preparePowerOff,&powerOffQuiescent,&enterManualPoweroff,&resumePowerOff};
#elif MT_HAS_XIAO_MANUAL_POWEROFF
  if(!owner_.preparePowerOff||!owner_.quiescentPowerOff||!owner_.resumePowerOff)return {};
  return {this,&xiaoManualWakeReady,&preparePowerOff,&powerOffQuiescent,&enterXiaoManualPoweroff,&resumePowerOff};
#else
  return {};
#endif
}
}
