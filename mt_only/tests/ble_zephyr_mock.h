// SPDX-License-Identifier: GPL-3.0-only
// Thread-capable host stand-ins for actual BleTransport callbacks, not the stack.
#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>
#include <functional>
#include <cassert>
#include "ble_runtime/WorkQueueMock.h"
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include <cerrno>
#include <sys/types.h>
struct MockQueue {size_t size,capacity;std::deque<std::vector<uint8_t>> entries;};
#define K_MSGQ_DEFINE(name,size,count,align) static MockQueue name{size,count,{}}
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
#include "ble_native_store/stubs/zephyr/kernel.h"
#else
#define K_MUTEX_DEFINE(name) static std::recursive_mutex name
#define K_NO_WAIT 0
#define K_FOREVER -1
#define K_MSEC(n) n
inline int k_mutex_lock(std::recursive_mutex *p,int timeout){
 if(timeout==K_NO_WAIT)return p->try_lock()?0:-EBUSY;
 p->lock();return 0;
}
inline void k_mutex_unlock(std::recursive_mutex *p){p->unlock();}
#endif
inline std::function<void(MockQueue*)> mockBeforePut;
inline int k_msgq_put(MockQueue *q,const void *p,int){if(mockBeforePut)mockBeforePut(q);if(q->entries.size()==q->capacity)return -ENOSPC;const auto *b=static_cast<const uint8_t*>(p);q->entries.emplace_back(b,b+q->size);return 0;}
inline int k_msgq_get(MockQueue *q,void *p,int){if(q->entries.empty())return -EAGAIN;memcpy(p,q->entries.front().data(),q->size);q->entries.pop_front();return 0;}
inline void k_msgq_purge(MockQueue *q){q->entries.clear();}
inline uint32_t k_msgq_num_used_get(const MockQueue *q){return uint32_t(q->entries.size());}
using atomic_t=std::atomic<int>;
inline int atomic_set(atomic_t *p,int v){return p->exchange(v);}
inline bool atomic_cas(atomic_t *p,int old,int value){return p->compare_exchange_strong(old,value);}
inline int atomic_get(const atomic_t *p){return p->load();}
inline int atomic_inc(atomic_t *p){return p->fetch_add(1);}
inline void sys_put_le32(uint32_t x,uint8_t *p){for(unsigned i=0;i<4;++i)p[i]=x>>(8*i);}
#define BT_SECURITY_L1 1
#define BT_SECURITY_L2 2
#define BT_SECURITY_L3 3
#define BT_PASSKEY_RAND 0xffffffffu
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define IS_ENABLED(value) (value)
using bt_security_t=uint8_t;
enum bt_security_err {BT_SECURITY_ERR_SUCCESS=0,BT_SECURITY_ERR_PIN_OR_KEY_MISSING=2,BT_SECURITY_ERR_UNSPECIFIED=9};
struct bt_conn_auth_cb;
struct bt_conn {
 std::atomic<unsigned> references{1};
 std::atomic<bt_security_t> security{BT_SECURITY_L1};
 const bt_conn_auth_cb *authentication=nullptr;
};
struct bt_uuid{unsigned type;};
struct bt_uuid_128{bt_uuid uuid;uint8_t val[16];};
#define BT_UUID_128_ENCODE(...) 0
#define BT_UUID_INIT_128(...) {{0},{0}}
#define BT_GATT_ERR(x) (-x)
#define BT_ATT_ERR_UNLIKELY 14
#define BT_ATT_ERR_AUTHENTICATION 5
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES 17
#define BT_GATT_WRITE_FLAG_PREPARE 1
#define BT_GATT_WRITE_FLAG_EXECUTE 2
#define BT_GATT_CHRC_WRITE 8
#define BT_GATT_CHRC_READ 2
#define BT_GATT_CHRC_NOTIFY 16
#define BT_GATT_PERM_WRITE_ENCRYPT 16
#define BT_GATT_PERM_READ_ENCRYPT 4
#define BT_GATT_PERM_PREPARE_WRITE 64
struct bt_gatt_attr {
 const void *uuid;
 ssize_t (*read)(bt_conn*,const bt_gatt_attr*,void*,uint16_t,uint16_t);
 ssize_t (*write)(bt_conn*,const bt_gatt_attr*,const void*,uint16_t,uint16_t,uint8_t);
 void (*ccc)(const bt_gatt_attr*,uint16_t);
 unsigned permissions;
};
#define BT_GATT_PRIMARY_SERVICE(u) {u,nullptr,nullptr,nullptr,0}
#define BT_GATT_CHARACTERISTIC(u,c,p,r,w,x) {u,nullptr,nullptr,nullptr,0},{u,r,w,nullptr,p}
#define BT_GATT_CCC(c,p) {nullptr,nullptr,nullptr,c,p}
#if defined(CONFIG_MT_BLE_LOG_RADIO)
#define BT_GATT_SERVICE_DEFINE(name,...) static struct {bt_gatt_attr attrs[11];} name{{__VA_ARGS__}}
#else
#define BT_GATT_SERVICE_DEFINE(name,...) static struct {bt_gatt_attr attrs[8];} name{{__VA_ARGS__}}
#endif
inline ssize_t bt_gatt_attr_read(bt_conn*,const bt_gatt_attr*,void *out,uint16_t max,uint16_t offset,const void *p,uint16_t n){if(offset>n)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);const auto count=std::min<unsigned>(max,n-offset);memcpy(out,static_cast<const uint8_t*>(p)+offset,count);return count;}
static int mockSecurityResult=0;
static int mockNotifyResult=0,mockNotifyCalls=0,mockAdvertiseResult=0,mockAdvertiseCalls=0,mockSecurity=0;
#if defined(CONFIG_MT_BLE_LOG_RADIO)
#define BT_GATT_CCC_NOTIFY 1
static bool mockLogSubscribed=false;
static unsigned mockAttBudget=UINT32_MAX;
inline std::function<void()> mockDuringSubscribe;
struct MockAttValue {bt_conn *peer;const bt_gatt_attr *attr;std::vector<uint8_t> data;};
inline std::vector<MockAttValue> mockAttDelivered;
static unsigned mockMtu=128;
static bt_conn *mockNotifyPeer=nullptr;
static const bt_gatt_attr *mockNotifyAttr=nullptr;
static std::vector<uint8_t> mockNotification;
inline bool bt_gatt_is_subscribed(bt_conn*,const bt_gatt_attr*,uint16_t flag){if(mockDuringSubscribe)mockDuringSubscribe();return flag==BT_GATT_CCC_NOTIFY&&mockLogSubscribed;}
inline unsigned bt_gatt_get_mtu(bt_conn*){return mockMtu;}
inline int bt_gatt_notify(bt_conn*peer,const bt_gatt_attr*attr,const void*data,uint16_t size){
 assert(mockSystemWorkqueue);++mockNotifyCalls;mockNotifyPeer=peer;mockNotifyAttr=attr;
 const auto*bytes=static_cast<const uint8_t*>(data);mockNotification.assign(bytes,bytes+size);
 if(mockNotifyResult)return mockNotifyResult;
 if(!mockAttBudget)return -ENOMEM;
 --mockAttBudget;mockAttDelivered.push_back({peer,attr,mockNotification});return 0;
}
#else
inline int bt_gatt_notify(bt_conn*,const bt_gatt_attr*,const void*,uint16_t){assert(mockSystemWorkqueue);++mockNotifyCalls;return mockNotifyResult;}
#endif
inline std::function<void()> mockDuringSecurity;
inline bool mockAutoSecurity=true;
inline void (*mockSecurityChanged)(bt_conn*,bt_security_t,bt_security_err)=nullptr;
inline int bt_conn_set_security(bt_conn *conn,int n){
 mockSecurity=n;if(mockDuringSecurity)mockDuringSecurity();
 if(!mockSecurityResult&&mockAutoSecurity){
  conn->security=bt_security_t(n);
  if(mockSecurityChanged)mockSecurityChanged(conn,bt_security_t(n),BT_SECURITY_ERR_SUCCESS);
 }
 return mockSecurityResult;
}
inline bt_conn *bt_conn_ref(bt_conn *conn){assert(conn&&conn->references.load());++conn->references;return conn;}
inline void bt_conn_unref(bt_conn *conn){assert(conn&&conn->references.load()>1);--conn->references;}
inline bt_security_t bt_conn_get_security(const bt_conn *conn){return conn->security.load();}
inline int mockDisconnectResult=0,mockDisconnectCalls=0;
inline int bt_conn_disconnect(bt_conn*,uint8_t reason){assert(reason==BT_HCI_ERR_REMOTE_USER_TERM_CONN);++mockDisconnectCalls;return mockDisconnectResult;}
struct bt_conn_auth_cb {
 void (*passkey_display)(bt_conn*,unsigned int)=nullptr;
 void (*cancel)(bt_conn*)=nullptr;
 uint32_t (*app_passkey)(bt_conn*)=nullptr;
};
inline int mockOverlayResult=0,mockOverlayCalls=0;
inline int bt_conn_auth_cb_overlay(bt_conn *conn,const bt_conn_auth_cb *authentication){++mockOverlayCalls;if(!mockOverlayResult)conn->authentication=authentication;return mockOverlayResult;}
#define BT_CONN_CB_DEFINE(name) static struct {void (*connected)(bt_conn*,uint8_t);void (*disconnected)(bt_conn*,uint8_t);void (*security_changed)(bt_conn*,bt_security_t,bt_security_err);} name
#define BT_LE_AD_GENERAL 2
#define BT_LE_AD_NO_BREDR 4
#define BT_DATA_FLAGS 1
#define BT_DATA_UUID128_ALL 7
#define BT_DATA_NAME_COMPLETE 9
struct bt_data{int type;const void *data;uint8_t length;};
#define BT_DATA(t,d,n) {t,d,n}
#define BT_LE_ADV_CONN_FAST_1 nullptr
static char mockName[40];static bool mockBondable=true;
inline const char *bt_get_name(){return mockName;}
inline int bt_set_name(const char *s){strcpy(mockName,s);return 0;}
inline void bt_set_bondable(bool b){mockBondable=b;}
inline int bt_enable(void*){return 0;}
inline int bt_le_adv_start(void*,const bt_data*,size_t,const bt_data*,size_t){++mockAdvertiseCalls;return mockAdvertiseResult;}
inline int mockAdvertisingStopResult=0,mockAdvertisingStopCalls=0;
inline int bt_le_adv_stop(){++mockAdvertisingStopCalls;return mockAdvertisingStopResult;}

#if !defined(CONFIG_MT_BLE_NATIVE_STORE)
inline uint32_t mockUptime=0;
inline uint32_t k_uptime_get_32(){return mockUptime;}
inline int64_t k_uptime_get(){return mockUptime;}
#endif
struct bt_conn_auth_info_cb {
 void (*pairing_complete)(bt_conn*,bool);
 void (*pairing_failed)(bt_conn*,bt_security_err);
};
inline int mockAuthRegisterResult=0;
inline bt_conn_auth_info_cb *mockAuthCallbacks=nullptr;
inline int bt_conn_auth_info_cb_register(bt_conn_auth_info_cb *p){if(!mockAuthRegisterResult)mockAuthCallbacks=p;return mockAuthRegisterResult;}
