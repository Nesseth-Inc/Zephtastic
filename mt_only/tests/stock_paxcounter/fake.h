#pragma once
#include <stdint.h>
#include <errno.h>
#define DT_HAS_ALIAS(x) 0
struct k_spinlock{};using k_spinlock_key_t=int;
inline k_spinlock_key_t k_spin_lock(k_spinlock*){return 0;}
inline void k_spin_unlock(k_spinlock*,int){}
inline uint32_t testNow;inline uint32_t k_uptime_get_32(){return testNow;}
struct bt_addr_le_t {struct {uint8_t val[6];}a;};struct bt_le_scan_recv_info {bt_addr_le_t*addr;int8_t rssi;};
struct net_buf_simple{};
struct bt_le_scan_cb {void(*recv)(const bt_le_scan_recv_info*,net_buf_simple*);};
struct bt_le_scan_param {int type,options,interval,window,timeout,interval_coded,window_coded;};
enum{BT_LE_SCAN_TYPE_PASSIVE=1,BT_GAP_SCAN_FAST_INTERVAL=2,BT_GAP_SCAN_FAST_WINDOW=3};
inline bt_le_scan_cb* testBleCb;inline bool testBleRunning;
inline int bt_le_scan_cb_register(bt_le_scan_cb*v){testBleCb=v;return 0;}
inline void bt_le_scan_cb_unregister(bt_le_scan_cb*){testBleCb=nullptr;}
inline int bt_le_scan_start(bt_le_scan_param*,void*){if(testBleRunning)return -EALREADY;testBleRunning=true;return 0;}
inline int bt_le_scan_stop(){testBleRunning=false;return 0;}
enum wifi_promiscuous_pkt_type_t {WIFI_PKT_MGMT,WIFI_PKT_DATA,WIFI_PKT_CTRL};
struct wifi_promiscuous_pkt_t {struct {int sig_len;int8_t rssi;}rx_ctrl;uint8_t payload[24];};
enum wifi_mode_t {ESP32_WIFI_MODE_NULL,STA};enum wifi_second_chan_t {WIFI_SECOND_CHAN_NONE};
struct wifi_promiscuous_filter_t {uint32_t filter_mask;};struct wifi_country_t{uint8_t schan,nchan;};
enum {ESP_OK=0,WIFI_PROMIS_FILTER_MASK_MGMT=1,WIFI_PROMIS_FILTER_MASK_DATA=2};
inline bool testMonitor=false;inline int testFailure=0;inline wifi_mode_t testMode=ESP32_WIFI_MODE_NULL;
inline uint8_t testChannel=9;inline wifi_country_t testCountry{3,3};inline wifi_promiscuous_filter_t testFilter{15};
using Callback=void(*)(void*,wifi_promiscuous_pkt_type_t);inline Callback testCallback;
inline int esp_wifi_get_mode(wifi_mode_t*r){*r=testMode;return 0;}
inline int esp_wifi_get_country(wifi_country_t*r){*r=testCountry;return 0;}
inline int esp_wifi_get_promiscuous(bool*r){*r=testMonitor;return 0;}
inline int esp_wifi_get_channel(uint8_t*r,wifi_second_chan_t*s){*r=testChannel;*s=WIFI_SECOND_CHAN_NONE;return 0;}
inline int esp_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t*r){*r=testFilter;return 0;}
inline int esp_wifi_set_promiscuous_filter(wifi_promiscuous_filter_t*r){if(testFailure==1)return -1;testFilter=*r;return 0;}
inline int esp_wifi_set_promiscuous_rx_cb(Callback c){if(testFailure==2)return -1;testCallback=c;return 0;}
inline int esp_wifi_set_channel(uint8_t c,wifi_second_chan_t){if(testFailure==3)return -1;testChannel=c;return 0;}
inline int esp_wifi_set_promiscuous(bool b){if(testFailure==4)return -1;testMonitor=b;return 0;}
