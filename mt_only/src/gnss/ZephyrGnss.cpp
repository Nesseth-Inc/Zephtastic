// SPDX-License-Identifier: MIT
// ZephCore GPS-manager adaptation: three good fixes, immutable handoff,
// board-specific standby/power and manual receiver PM on a dedicated worker.
#include "ZephyrGnss.h"
#include "GnssAcquisitionGate.h"
#include "GnssPromotionProjection.h"
#include "GnssMeasuredProjection.h"
#include "GnssUtcTime.h"
#include "GnssClockProjection.h"
#if defined(CONFIG_MT_GNSS_T096)
#include "T096GnssPower.h"
#else
#include "L76kStandby.h"
#if defined(CONFIG_MT_GNSS_L76K_SETUP)
#include "L76kSetup.h"
#include <zephyr/drivers/uart.h>
#endif
#endif
#include <zephyr/kernel.h>
#include <zephyr/drivers/gnss.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/pm/device.h>
#include <errno.h>
extern "C" uint32_t zephcore_gnss_rx_count(void);
extern "C" bool zt_gnss_nmea0183_match_has_bearing(const gnss_data *data);
namespace mt::gnss {
namespace {
const device *const receiver=DEVICE_DT_GET(DT_NODELABEL(gnss));
const device *const uart=DEVICE_DT_GET(DT_BUS(DT_NODELABEL(gnss)));
#if defined(CONFIG_MT_GNSS_T096)
const device *const rail=DEVICE_DT_GET(DT_NODELABEL(gps_power));
const gpio_dt_spec reset=GPIO_DT_SPEC_GET(DT_ALIAS(gps_reset),gpios);
#else
const gpio_dt_spec standby=GPIO_DT_SPEC_GET(DT_PATH(zephyr_user),gnss_standby_gpios);
BUILD_ASSERT(!DT_SAME_NODE(DT_BUS(DT_NODELABEL(gnss)), DT_CHOSEN(zephyr_console)),
             "GNSS must not take the console UART");
#endif
K_MUTEX_DEFINE(fixLock);
zt::gnss::PendingPromotion pending{};
zt::gnss::AcquisitionGate gate;
zt::gnss::PromotionEvent latest{},published{};
GnssSettings requested{};
bool requestedFixed=false;
Diagnostics publishedState{};
uint32_t requestEpoch=0;
K_SEM_DEFINE(firstPoll,0,1);
#if defined(CONFIG_MT_GNSS_T096)
zt::gnss::t096::State power{};
#endif
bool initialized=false,available=false,accepting=false,fault=false,acquiring=false;
uint64_t started=0,nextWake=0;

int pm(const device *dev,bool on) {
  pm_device_state state;int result=pm_device_state_get(dev,&state);if(result)return result;
  const auto desired=on?PM_DEVICE_STATE_ACTIVE:PM_DEVICE_STATE_SUSPENDED;
  if(state==desired)return 0;
  return pm_device_action_run(dev,on?PM_DEVICE_ACTION_RESUME:PM_DEVICE_ACTION_SUSPEND);
}
#if defined(CONFIG_MT_GNSS_T096)
int setRail(void*,bool on){
  if(regulator_is_enabled(rail)==on)return 0;
  return on?regulator_enable(rail):regulator_disable(rail);
}
int setReset(void*,bool asserted){return gpio_pin_set_dt(&reset,asserted);}
int passiveUart(void*,bool on) {
  if(on){
    const int result=pm(uart,true);if(result)return result;
    const int opened=pm(receiver,true);if(opened)(void)pm(uart,false);return opened;
  }
  // Stop modem pipe before suspending UART; callers run on the GNSS worker.
  const int closed=pm(receiver,false);if(closed)return closed;
  return pm(uart,false);
}
const zt::gnss::t096::Ops operations{nullptr,setRail,setReset,passiveUart};
#endif
#if defined(CONFIG_MT_GNSS_L76K_SETUP)
bool prepared=false;
struct SetupUart {
  uint32_t epoch;
  bool active() const {
    k_mutex_lock(&fixLock,K_FOREVER);
    const bool ok=epoch==requestEpoch && requested.enabled && !requestedFixed;
    k_mutex_unlock(&fixLock);
    return ok;
  }
  uint32_t now() const {return k_uptime_get_32();}
  bool wait(uint32_t ms) const {
    const uint32_t start=now();
    while(active()){
      const uint32_t elapsed=now()-start;
      if(elapsed>=ms)return true;
      k_sleep(K_MSEC(MIN(ms-elapsed,10u)));
    }
    return false;
  }
  bool speed(uint32_t rate) const {
    uart_config cfg{};
    if(uart_config_get(uart,&cfg))return false;
    if(cfg.baudrate==rate)return true;
    cfg.baudrate=rate;
    return uart_configure(uart,&cfg)==0;
  }
  bool read(char &c) const {
    unsigned char byte;
    if(uart_poll_in(uart,&byte))return false;
    c=static_cast<char>(byte);
    return true;
  }
  bool send(const char *cmd) const {
    if(!active())return false;
    while(*cmd)uart_poll_out(uart,static_cast<unsigned char>(*cmd++));
    return true;
  }
};
bool prepareReceiver(){
  k_mutex_lock(&fixLock,K_FOREVER);
  SetupUart io{requestEpoch};
  k_mutex_unlock(&fixLock);
  if(!io.active())return false;
  if(gpio_pin_set_dt(&standby,false)){fault=true;return false;}
  const auto result=l76k::setup(io);
  if(result==l76k::Result::Configured || result==l76k::Result::Passive){
    prepared=true;
    if(io.active())return true;
  }
  if(gpio_pin_set_dt(&standby,true) || result==l76k::Result::Error)fault=true;
  return false;
}
#endif
void enableCallbacks(bool enabled) {
  k_mutex_lock(&fixLock,K_FOREVER);accepting=enabled;gate.reset();pending={};
  k_mutex_unlock(&fixLock);
}
bool transition(bool on) {
  enableCallbacks(false);
  bool ok;
#if defined(CONFIG_MT_GNSS_T096)
  const auto result=on?zt::gnss::t096::start(operations,power):zt::gnss::t096::stop(operations,power);
  power=result.final;
  ok=result.validation==zt::gnss::t096::Validation::kOk&&!result.op_error&&!result.rollback_error;
#else
  ok=setL76kAcquiring(on,[](bool asserted){return gpio_pin_set_dt(&standby,asserted);},
                         [](bool open){return pm(receiver,open);});
#endif
  if(!ok){fault=true;latest={};return false;}
  acquiring=on;enableCallbacks(on);return true;
}
void onData(const device*,const gnss_data *data) {
  k_mutex_lock(&fixLock,K_FOREVER);
  ++publishedState.callbacks;publishedState.satellites=data->info.satellites_cnt;
  zt::gnss::PromotionEvent candidate{data->nav_data.latitude,data->nav_data.longitude,k_uptime_get(),
    data->nav_data.altitude,data->info.satellites_cnt,data->info.fix_status>=GNSS_FIX_STATUS_GNSS_FIX};
  candidate.measured=true;candidate.speedMmPerSecond=data->nav_data.speed;candidate.geoidMm=data->info.geoid_separation;
  candidate.hdopMilli=data->info.hdop;
  if(zt_gnss_nmea0183_match_has_bearing(data)&&data->nav_data.bearing<=359999u) {
    candidate.groundTrackE5=data->nav_data.bearing*100u;
    candidate.hasGroundTrack=true;
  }
  const auto &utc=data->utc;
  candidate.hasUtc=zt::gnss::toUnixSeconds({utc.century_year,utc.month,utc.month_day,utc.hour,utc.minute,utc.millisecond},candidate.utcSeconds);
  if(gate.consider(candidate,accepting&&requested.enabled&&!requestedFixed,pending))++publishedState.promotions;
  k_mutex_unlock(&fixLock);
}
GNSS_DT_DATA_CALLBACK_DEFINE(DT_NODELABEL(gnss),onData);
void init() {
  initialized=true;
  if(!device_is_ready(receiver)||!device_is_ready(uart))return;
#if defined(CONFIG_MT_GNSS_T096)
  if(!device_is_ready(rail)||!gpio_is_ready_dt(&reset))return;
  if(gpio_pin_configure_dt(&reset,GPIO_OUTPUT_ACTIVE)||passiveUart(nullptr,false))return;
  power={true,regulator_is_enabled(rail),true,true,true,false};
#else
  if(!gpio_is_ready_dt(&standby)||gpio_pin_configure_dt(&standby,GPIO_OUTPUT_ACTIVE))return;
#endif
  available=transition(false);
}
}
static void pollWorker(const GnssSettings &settings,bool fixed,uint64_t now) {
  if(!initialized)init();
  if(!available||fault)return;
  if(!settings.enabled||fixed){
    if(acquiring)(void)transition(false);
    enableCallbacks(false);latest={};nextWake=0;return;
  }
  if(!acquiring){
    if(now<nextWake)return;
#if defined(CONFIG_MT_GNSS_L76K_SETUP)
    if(!prepared){
      if(!prepareReceiver())return;
      // Probe time is outside the configured live-fix acquisition interval.
      now=uint64_t(k_uptime_get());
    }
#endif
    if(!transition(true))return;
    started=now;
  }
  zt::gnss::PromotionEvent fix{};
  k_mutex_lock(&fixLock,K_FOREVER);const bool obtained=zt::gnss::takePromotion(pending,fix);k_mutex_unlock(&fixLock);
  if(obtained)latest=fix;
  // Pinned protocol zero update means30s; deprecated attempt zero resolves to
  // the regular broadcast interval (15min default). Full uint32 fields use64bit.
  const uint64_t attempt=uint64_t(settings.attemptSecs?settings.attemptSecs:settings.broadcastSecs?settings.broadcastSecs:900)*1000;
  if(obtained||now-started>=attempt){
    if(transition(false))nextWake=now+uint64_t(settings.updateSecs?settings.updateSecs:30)*1000;
  }
}
namespace {
void worker(void*,void*,void*) {
  k_sem_take(&firstPoll,K_FOREVER);
  uint32_t epoch=0;
  for(;;) {
    k_mutex_lock(&fixLock,K_FOREVER);const auto settings=requested;const bool fixed=requestedFixed;
    const uint32_t observedEpoch=requestEpoch;k_mutex_unlock(&fixLock);
    if(epoch!=observedEpoch){
      latest={};enableCallbacks(false);
      if(acquiring)(void)transition(false);
      epoch=observedEpoch;
    }
    pollWorker(settings,fixed,uint64_t(k_uptime_get()));
    k_mutex_lock(&fixLock,K_FOREVER);
    publishedState.initialized=initialized;publishedState.ready=available&&!fault;
    publishedState.fault=fault;publishedState.acquiring=acquiring;
    published=requestEpoch==epoch&&requested.enabled&&!requestedFixed?latest:zt::gnss::PromotionEvent{};
    k_mutex_unlock(&fixLock);
    k_sleep(K_MSEC(100));
  }
}
// The modem pipe waits up to10s. Keep those waits off the8s watchdog-fed main
// thread AND off the shared workqueue that completes the modem operations.
K_THREAD_DEFINE(gnssWorker,2048,worker,nullptr,nullptr,nullptr,8,0,0);
}
Diagnostics diagnostics(){
  k_mutex_lock(&fixLock,K_FOREVER);auto out=publishedState;k_mutex_unlock(&fixLock);
  out.sentences=zephcore_gnss_rx_count();return out;
}
void poll(const GnssSettings &settings,bool fixed,uint64_t) {
  k_mutex_lock(&fixLock,K_FOREVER);
  if(requested.enabled!=settings.enabled||requestedFixed!=fixed)++requestEpoch;
  requested=settings;requestedFixed=fixed;
  if(!settings.enabled||fixed){accepting=false;pending={};published={};}
  k_mutex_unlock(&fixLock);k_sem_give(&firstPoll);
}
meshtastic_Position snapshot(uint64_t now) {
  k_mutex_lock(&fixLock,K_FOREVER);const auto fix=published;const auto settings=requested;
  const bool enabled=settings.enabled&&!requestedFixed; k_mutex_unlock(&fixLock);
  meshtastic_Position position{};
  if(!enabled||!zt::gnss::projectPosition(fix,int64_t(now),120000,position))return {};
  // The checked native parser supplies one matched GGA/RMC sample, including
  // measurement presence. Failed/void input cannot inherit old navigation.
  zt::gnss::projectMeasurements(fix,settings.positionFlags,position);
  return position;
}
uint32_t utcSnapshot(uint64_t now){
  if(now>INT64_MAX)return 0;
  k_mutex_lock(&fixLock,K_FOREVER);const auto fix=published;
  const bool enabled=requested.enabled&&!requestedFixed;k_mutex_unlock(&fixLock);
  return enabled?zt::gnss::projectUtc(fix,int64_t(now),120000):0;
}
}
