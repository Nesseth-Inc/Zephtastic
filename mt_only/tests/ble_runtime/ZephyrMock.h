// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#include <cassert>
#include "WorkQueueMock.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include <sys/types.h>
inline unsigned sessionLockDepth=0;
struct k_mutex{};
#define K_MUTEX_DEFINE(name) static k_mutex name
#define K_FOREVER 0
#define K_NO_WAIT 0
inline int k_mutex_lock(k_mutex*,int){++sessionLockDepth;return 0;}
inline int k_mutex_unlock(k_mutex*){assert(sessionLockDepth);--sessionLockDepth;return 0;}
inline int64_t mockNow=123;
inline int64_t k_uptime_get(){return mockNow;}
inline uint32_t k_uptime_get_32(){return static_cast<uint32_t>(mockNow);}
struct k_msgq{size_t size,capacity;std::deque<std::vector<uint8_t>> values;};
#define K_MSGQ_DEFINE(name,size,capacity,alignment) static k_msgq name{size,capacity,{}}
inline void k_msgq_purge(k_msgq*q){q->values.clear();}
inline uint32_t k_msgq_num_used_get(k_msgq*q){return q?static_cast<uint32_t>(q->values.size()):0;}
inline int k_msgq_put(k_msgq*q,const void*p,int){if(q->values.size()==q->capacity)return -ENOMEM;q->values.emplace_back(static_cast<const uint8_t*>(p),static_cast<const uint8_t*>(p)+q->size);return 0;}
inline int k_msgq_get(k_msgq*q,void*p,int){if(q->values.empty())return -EAGAIN;memcpy(p,q->values.front().data(),q->size);q->values.pop_front();return 0;}
using atomic_t=int;
inline bool atomic_cas(atomic_t*p,int old,int value){if(*p!=old)return false;*p=value;return true;}
inline int atomic_get(const atomic_t*p){return *p;}
inline int atomic_set(atomic_t*p,int n){const int old=*p;*p=n;return old;}
inline int atomic_inc(atomic_t*p){return (*p)++;}
inline void sys_put_le32(uint32_t n,uint8_t*p){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
#define IS_ENABLED(value) (value)
struct bt_conn;
enum bt_security_t:uint8_t{BT_SECURITY_L1=1,BT_SECURITY_L2=2,BT_SECURITY_L3=3};
enum bt_security_err:uint8_t{BT_SECURITY_ERR_SUCCESS=0,BT_SECURITY_ERR_AUTH_FAIL=1,BT_SECURITY_ERR_PIN_OR_KEY_MISSING=2};
struct bt_conn_auth_cb{void(*passkey_display)(bt_conn*,unsigned)=nullptr;void(*cancel)(bt_conn*)=nullptr;uint32_t(*app_passkey)(bt_conn*)=nullptr;};
struct bt_conn_auth_info_cb{void(*pairing_complete)(bt_conn*,bool)=nullptr;void(*pairing_failed)(bt_conn*,bt_security_err)=nullptr;};
struct bt_conn_cb{void(*connected)(bt_conn*,uint8_t);void(*disconnected)(bt_conn*,uint8_t);void(*security_changed)(bt_conn*,bt_security_t,bt_security_err);};
struct bt_conn{bt_security_t level=BT_SECURITY_L1,requested=BT_SECURITY_L1;const bt_conn_auth_cb*auth=nullptr;unsigned refs=1;};
#define BT_CONN_CB_DEFINE(name) static bt_conn_cb name
#define BT_PASSKEY_RAND 0xffffffffu
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 19
inline bool mockAdvertising=false,mockBondable=true;
inline unsigned mockNotifies=0,mockDisconnects=0,mockAdvertisingStarts=0,mockStackCalls=0;
inline bt_conn*mockLastNotify=nullptr;
inline bt_conn*mockLastDisconnect=nullptr;
inline int mockOverlayError=0,mockSecurityError=0,mockAdvError=0;
inline bt_conn_auth_info_cb*mockAuthInfo=nullptr;
inline bt_conn*bt_conn_ref(bt_conn*p){++p->refs;return p;}
inline void bt_conn_unref(bt_conn*p){assert(p&&p->refs>0);--p->refs;}
inline bt_security_t bt_conn_get_security(bt_conn*p){return p->level;}
inline void stackBoundary(){assert(sessionLockDepth==0);++mockStackCalls;}
inline int bt_enable(void*){stackBoundary();return 0;}
inline void bt_set_bondable(bool b){stackBoundary();mockBondable=b;}
inline int bt_conn_auth_info_cb_register(bt_conn_auth_info_cb*p){stackBoundary();mockAuthInfo=p;return 0;}
inline int bt_conn_auth_cb_overlay(bt_conn*p,const bt_conn_auth_cb*a){stackBoundary();p->auth=a;return mockOverlayError;}
inline int bt_conn_set_security(bt_conn*p,bt_security_t s){stackBoundary();assert(p->auth);p->requested=s;return mockSecurityError;}
inline int bt_conn_disconnect(bt_conn*conn,int){stackBoundary();mockLastDisconnect=conn;++mockDisconnects;return 0;}
inline int bt_set_name(const char*){stackBoundary();return 0;}
inline const char*bt_get_name(){stackBoundary();return "mock";}
struct bt_data{uint8_t type;const void*data;uint8_t size;};
#define BT_DATA(type,data,size) {type,data,size}
#define BT_DATA_FLAGS 1
#define BT_DATA_UUID128_ALL 2
#define BT_DATA_NAME_COMPLETE 3
#define BT_LE_AD_GENERAL 1
#define BT_LE_AD_NO_BREDR 2
#define BT_LE_ADV_CONN_FAST_1 nullptr
inline int bt_le_adv_start(void*,const bt_data*,int,const bt_data*,int){stackBoundary();++mockAdvertisingStarts;if(mockAdvError)return mockAdvError;mockAdvertising=true;return 0;}
inline int bt_le_adv_stop(){stackBoundary();mockAdvertising=false;return 0;}
struct bt_uuid{int unused;};
struct bt_uuid_128{bt_uuid uuid;uint8_t val[16];};
#define BT_UUID_128_ENCODE(...) 0
#define BT_UUID_INIT_128(...) {{0},{0}}
struct bt_gatt_attr{};
struct bt_gatt_service{bt_gatt_attr attrs[8];};
#define BT_GATT_SERVICE_DEFINE(name,...) static bt_gatt_service name{}
#define BT_GATT_ERR(code) (-int(code))
#define BT_ATT_ERR_AUTHENTICATION 5
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES 17
#define BT_GATT_WRITE_FLAG_PREPARE 1
inline ssize_t bt_gatt_attr_read(bt_conn*,const bt_gatt_attr*,void*out,uint16_t n,uint16_t offset,const void*data,size_t size){if(offset>size)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);size_t count=size-offset;if(count>n)count=n;memcpy(out,static_cast<const uint8_t*>(data)+offset,count);return ssize_t(count);}
inline int bt_gatt_notify(bt_conn*conn,const bt_gatt_attr*,const void*,size_t){assert(mockSystemWorkqueue);stackBoundary();mockLastNotify=conn;++mockNotifies;return 0;}
