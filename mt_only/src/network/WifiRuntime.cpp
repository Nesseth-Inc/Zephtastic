// SPDX-License-Identifier: GPL-3.0-only
#include "WifiRuntime.h"
#include "WifiBootSnapshot.h"
#if defined(CONFIG_MT_MQTT_GATEWAY)
#include "MqttRuntime.h"
#endif
#include <zephyr/kernel.h>
#if defined(CONFIG_MT_WIFI_STATION)
#include "ZephyrWiFiStation.h"
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/atomic.h>
#endif
namespace mt::network {
static WifiBootSnapshot boot;
static WifiStatus observed;
#if defined(CONFIG_MT_WIFI_STATION)
static atomic_t pendingTime;
static void synced(uint32_t seconds){
  if(!seconds)return;
#if defined(CONFIG_MT_MQTT_GATEWAY)
  (void)mt::mqtt::syncCertificateTime(seconds);
#endif
  atomic_set(&pendingTime,atomic_val_t(seconds));
}
#endif
bool wifiBootActive(){return boot.active();}
bool wifiAvailable(){return IS_ENABLED(CONFIG_MT_WIFI_STATION);}
bool wifiStart(const WifiStationSettings &settings,bool settingsReady){
  if(!settingsReady||!boot.prepare(settings,wifiAvailable()))return false;
#if defined(CONFIG_MT_WIFI_STATION)
  if(boot.active())zc_wifi_station_start_ntp(boot.ssid(),boot.psk(),boot.ntpServer(),synced);
#endif
  return true;
}
void wifiPoll(){
  observed={};
#if defined(CONFIG_MT_WIFI_STATION)
  if(!boot.active())return;
  char address[16]{};struct net_in_addr ip{};
  if(!zc_wifi_station_ip(address,sizeof(address))||net_addr_pton(NET_AF_INET,address,&ip))return;
  // DHCP can supply a usable address while ZephCore is still attempting SNTP.
  observed.connected=true;
  observed.mqttReady=zc_wifi_station_is_connected();
  memcpy(&observed.ip,&ip,sizeof(observed.ip));
  memcpy(observed.ssid,boot.ssid(),strlen(boot.ssid())+1);
#endif
}
WifiStatus wifiStatus(){return observed;}
bool wifiTakeTime(uint32_t &seconds){
  seconds=0;
#if defined(CONFIG_MT_WIFI_STATION)
  seconds=uint32_t(atomic_set(&pendingTime,0));
#endif
  return seconds!=0;
}
}
