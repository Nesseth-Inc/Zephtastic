// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrPeripherals.h"
#include "PaxcounterCore.h"

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_BT_OBSERVER)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#endif

#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
#include <esp_err.h>
#include <esp_wifi.h>
#endif

namespace mt::modules::peripherals::zephyr {
namespace {
constexpr size_t ObservationQueueCapacity = 128;
struct PendingObservation {
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  uint16_t bucket=0;
#else
  uint8_t address[6]{};
#endif
  int8_t rssi=0;
  PaxcounterCore::Kind kind{}; // Queue publication assigns the observed transport.
  uint32_t timeMs=0;
};
static PendingObservation observations[ObservationQueueCapacity]{};
static size_t queueRead=0, queueWrite=0, queueCount=0;
static uint32_t queueDrops=0;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
static bool capture=false;
#endif
static struct k_spinlock queueLock;

static void queueObservation(PaxcounterCore::Kind kind, const uint8_t address[6],
                             int8_t rssi, uint32_t timeMs) {
  if (!address) return;
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  uint16_t id;
  if(!stock::paxcounter::Interval::bucket(address,kind==PaxcounterCore::Kind::Ble,id))return;
#endif
  k_spinlock_key_t key=k_spin_lock(&queueLock);
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  if(!capture){k_spin_unlock(&queueLock,key);return;}
#endif
  if(queueCount==ObservationQueueCapacity) {
    if(queueDrops!=UINT32_MAX)++queueDrops;
  } else {
    PendingObservation &entry=observations[queueWrite];
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    entry.bucket=id;
#else
    memcpy(entry.address,address,sizeof(entry.address));
#endif
    entry.kind=kind; entry.rssi=rssi; entry.timeMs=timeMs;
    queueWrite=(queueWrite+1)%ObservationQueueCapacity;
    ++queueCount;
  }
  k_spin_unlock(&queueLock,key);
}

#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_BT_OBSERVER)
static bool bleCallbackRegistered=false;
static bool bleScanOwned=false;
static bool bleObserverActive=false;
static bool bleScanParamsReady=false;
static bt_le_scan_param bleScanParams{};
static void onBleReport(const bt_le_scan_recv_info *info, struct net_buf_simple *) {
  if(!info || !info->addr) return;
  queueObservation(PaxcounterCore::Kind::Ble, info->addr->a.val,
                   info->rssi, k_uptime_get_32());
}
static bt_le_scan_cb bleCallbacks{};
#endif

#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
static bool wifiPromiscOwned=false, filterSet=false, callbackSet=false, channelSet=false;
static uint8_t originalChannel=0, firstChannel=0, lastChannel=0, currentChannel=0;
static wifi_second_chan_t originalSecondary=WIFI_SECOND_CHAN_NONE;
static wifi_promiscuous_filter_t originalFilter{};
static uint32_t lastHop=0;
static void onWifiFrame(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if(!buffer || (type!=WIFI_PKT_MGMT&&type!=WIFI_PKT_DATA))return;
  const auto *packet=static_cast<const wifi_promiscuous_pkt_t *>(buffer);
  if(packet->rx_ctrl.sig_len<24)return;
  queueObservation(PaxcounterCore::Kind::Wifi,packet->payload+10,packet->rx_ctrl.rssi,k_uptime_get_32());
}
#endif

#if DT_HAS_ALIAS(rgb_red) && DT_HAS_ALIAS(rgb_green) && DT_HAS_ALIAS(rgb_blue)
static const pwm_dt_spec redPwm = PWM_DT_SPEC_GET(DT_ALIAS(rgb_red));
static const pwm_dt_spec greenPwm = PWM_DT_SPEC_GET(DT_ALIAS(rgb_green));
static const pwm_dt_spec bluePwm = PWM_DT_SPEC_GET(DT_ALIAS(rgb_blue));
static bool ready = false;
static int set(const pwm_dt_spec &spec, uint8_t value, uint8_t current) {
  if (!pwm_is_ready_dt(&spec)) return -ENODEV;
  const uint32_t duty = (uint32_t(spec.period) * value * current) / (255u * 255u);
  return pwm_set_pulse_dt(&spec, duty);
}
#endif
}

void init() {
#if DT_HAS_ALIAS(rgb_red) && DT_HAS_ALIAS(rgb_green) && DT_HAS_ALIAS(rgb_blue)
  ready = pwm_is_ready_dt(&redPwm) && pwm_is_ready_dt(&greenPwm) && pwm_is_ready_dt(&bluePwm);
#endif
}
bool ambientAvailable() {
#if DT_HAS_ALIAS(rgb_red) && DT_HAS_ALIAS(rgb_green) && DT_HAS_ALIAS(rgb_blue)
  return ready;
#else
  return false;
#endif
}
int applyAmbient(void *, uint8_t current, uint8_t red, uint8_t green, uint8_t blue) {
#if DT_HAS_ALIAS(rgb_red) && DT_HAS_ALIAS(rgb_green) && DT_HAS_ALIAS(rgb_blue)
  if (!ready) return -ENODEV;
  int rc = set(redPwm, red, current); if (rc) return rc;
  rc = set(greenPwm, green, current); if (rc) return rc;
  return set(bluePwm, blue, current);
#else
  (void)current; (void)red; (void)green; (void)blue; return -ENOTSUP;
#endif
}
int ambientOff(void *) {
#if DT_HAS_ALIAS(rgb_red) && DT_HAS_ALIAS(rgb_green) && DT_HAS_ALIAS(rgb_blue)
  if (!ready) return -ENODEV;
  int rc=pwm_set_pulse_dt(&redPwm,0); if(rc)return rc;
  rc=pwm_set_pulse_dt(&greenPwm,0); if(rc)return rc;
  return pwm_set_pulse_dt(&bluePwm,0);
#else
  return -ENOTSUP;
#endif
}

bool startBleObserver(bool paxcounterEnabled) {
#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_BT_OBSERVER)
  if(!paxcounterEnabled) return false;
  if(bleCallbackRegistered) return bleObserverActive;
  bleCallbacks.recv=onBleReport;
  const int registerRc=bt_le_scan_cb_register(&bleCallbacks);
  if(registerRc!=0) return false;
  bleCallbackRegistered=true;
  bleObserverActive=true;
  if(!bleScanParamsReady) {
    bleScanParams.type=BT_LE_SCAN_TYPE_PASSIVE;
    // No controller duplicate filter: the core owns time-window de-dup and
    // must receive a device again after the prior observation expires.
    bleScanParams.options=0;
    bleScanParams.interval=BT_GAP_SCAN_FAST_INTERVAL;
    bleScanParams.window=BT_GAP_SCAN_FAST_WINDOW;
    bleScanParams.timeout=0;
    bleScanParams.interval_coded=0;
    bleScanParams.window_coded=0;
    bleScanParamsReady=true;
  }
  const int scanRc=bt_le_scan_start(&bleScanParams, nullptr);
  if(scanRc==0) {
    bleScanOwned=true;
    return true;
  }
  // -EALREADY means another owner is scanning. We can consume shared reports,
  // but this module has no lease and must never stop that scanner.
#if !defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  if(scanRc==-EALREADY) return true;
#endif
  bleObserverActive=false;
  bt_le_scan_cb_unregister(&bleCallbacks);
  bleCallbackRegistered=false;
  return false;
#else
  (void)paxcounterEnabled; return false;
#endif
}
void stopBleObserver() {
#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_BT_OBSERVER)
  bleObserverActive=false;
  if(bleScanOwned) bt_le_scan_stop();
  bleScanOwned=false;
  if(bleCallbackRegistered) bt_le_scan_cb_unregister(&bleCallbacks);
  bleCallbackRegistered=false;
#endif
}

void stopWifiObserver() {
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  // Persist each unfinished cleanup step across transient driver failures.
  if(wifiPromiscOwned){
    if(esp_wifi_set_promiscuous(false)!=ESP_OK)return;
    wifiPromiscOwned=false;
  }
  if(callbackSet){
    if(esp_wifi_set_promiscuous_rx_cb(nullptr)!=ESP_OK)return;
    callbackSet=false;
  }
  if(filterSet){
    if(esp_wifi_set_promiscuous_filter(&originalFilter)!=ESP_OK)return;
    filterSet=false;
  }
  if(channelSet){
    wifi_mode_t mode;
    if(esp_wifi_get_mode(&mode)!=ESP_OK)return;
    if(mode==ESP32_WIFI_MODE_NULL && esp_wifi_set_channel(originalChannel,originalSecondary)!=ESP_OK)return;
    channelSet=false;
  }
#endif
}
void startWifiObserver(bool enabled) {
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  wifi_mode_t mode;
  if(!enabled || esp_wifi_get_mode(&mode)!=ESP_OK || mode!=ESP32_WIFI_MODE_NULL){stopWifiObserver();return;}
  const uint32_t now=k_uptime_get_32();
  if(wifiPromiscOwned){
    if(now-lastHop>=500){
      const uint8_t next=currentChannel==lastChannel?firstChannel:currentChannel+1;
      if(esp_wifi_set_channel(next,WIFI_SECOND_CHAN_NONE)==ESP_OK)currentChannel=next;
      lastHop=now;
    }
    return;
  }
  if(callbackSet||filterSet||channelSet){stopWifiObserver();return;}
  wifi_country_t country{};
  bool busy;
  if(esp_wifi_get_promiscuous(&busy)!=ESP_OK||busy||
     esp_wifi_get_country(&country)!=ESP_OK||!country.schan||!country.nchan||
     country.schan+country.nchan-1>14||
     esp_wifi_get_channel(&originalChannel,&originalSecondary)!=ESP_OK||
     esp_wifi_get_promiscuous_filter(&originalFilter)!=ESP_OK)return;
  wifi_promiscuous_filter_t filter{};
  filter.filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT|WIFI_PROMIS_FILTER_MASK_DATA;
  firstChannel=country.schan;lastChannel=country.schan+country.nchan-1;
  if(esp_wifi_set_promiscuous_filter(&filter)!=ESP_OK)return;
  filterSet=true;
  if(esp_wifi_set_promiscuous_rx_cb(onWifiFrame)==ESP_OK){
    callbackSet=true;
    if(esp_wifi_set_channel(firstChannel,WIFI_SECOND_CHAN_NONE)==ESP_OK){
      channelSet=true;
      if(esp_wifi_set_promiscuous(true)==ESP_OK){
        wifiPromiscOwned=true;currentChannel=firstChannel;lastHop=now;return;
      }
    }
  }
  stopWifiObserver();
#else
  (void)enabled;
#endif
}
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
void capturePax(bool enabled) {
  k_spinlock_key_t key=k_spin_lock(&queueLock);
  capture=enabled;
  if(!enabled){queueRead=queueWrite=queueCount=0;}
  k_spin_unlock(&queueLock,key);
}
#endif

size_t drainPaxObservations(PaxcounterCore &core) {
  size_t drained=0;
  while(drained<ObservationQueueCapacity) {
    PendingObservation item{};
    k_spinlock_key_t key=k_spin_lock(&queueLock);
    if(!queueCount) { k_spin_unlock(&queueLock,key); break; }
    item=observations[queueRead];
    observations[queueRead]=PendingObservation{};
    queueRead=(queueRead+1)%ObservationQueueCapacity;
    --queueCount;
    k_spin_unlock(&queueLock,key);
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
    core.observeBucket(item.kind,item.bucket,item.rssi,item.timeMs);
#else
    core.observe(item.kind,item.address,item.rssi,item.timeMs);
#endif
    ++drained;
  }
  return drained;
}
uint32_t observationQueueDrops() {
  k_spinlock_key_t key=k_spin_lock(&queueLock);
  const uint32_t result=queueDrops;
  k_spin_unlock(&queueLock,key);
  return result;
}
bool wifiObservationAvailable() {
#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
  return wifiPromiscOwned;
#else
  return false;
#endif
}
bool bleObservationAvailable() {
#if defined(CONFIG_MT_PAXCOUNTER) && defined(CONFIG_BT_OBSERVER)
  return bleObserverActive && bleCallbackRegistered;
#else
  return false;
#endif
}
} // namespace mt::modules::peripherals::zephyr
