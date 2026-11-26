// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ble_zephyr_mock.h"
#define BT_ATT_ERR_CCC_IMPROPER_CONF 0xfd
#define BT_ATT_ERR_VALUE_NOT_ALLOWED 0x13
#define BT_GATT_PERM_NONE 0
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP 4
#define BT_UUID_GATT_CCC nullptr
#define BT_GATT_DESCRIPTOR(u,p,r,w,d) {u,r,w,nullptr,p}
struct bt_gatt_service {bt_gatt_attr *attrs;size_t attr_count;};
#define BT_GATT_SERVICE(a) {a,sizeof(a)/sizeof(a[0])}
inline bt_gatt_service *fixtureDb=nullptr;
inline unsigned registrations=0,removals=0;
inline int bt_gatt_service_register(bt_gatt_service *s){
  assert(mockSystemWorkqueue&&!fixtureDb);fixtureDb=s;++registrations;return 0;
}
inline int bt_gatt_service_unregister(bt_gatt_service *s){
  assert(mockSystemWorkqueue&&fixtureDb==s);fixtureDb=nullptr;++removals;return 0;
}
struct bt_le_conn_param {uint16_t interval_min,interval_max,latency,timeout;};
inline bt_conn *parameterPeer;
inline int parameterResult;
inline unsigned parameterRequests;
inline int bt_conn_le_param_update(bt_conn *conn,const bt_le_conn_param *p){
  assert(mockSystemWorkqueue&&fixtureDb&&conn==parameterPeer&&conn->security>=BT_SECURITY_L2);
  assert(p->interval_min==12&&p->interval_max==12&&p->latency==0&&p->timeout==400);
  ++parameterRequests;return parameterResult;
}
struct bt_gatt_notify_params {
  const bt_gatt_attr *attr;
  const void *data;
  uint16_t len;
  void (*func)(bt_conn *,void *);
  void *user_data;
};
inline bt_gatt_notify_params queuedNotification{};
inline int bt_gatt_notify_cb(bt_conn *conn,bt_gatt_notify_params *params){
  int result=bt_gatt_notify(conn,params->attr,params->data,params->len);
  if(!result){assert(!queuedNotification.func);queuedNotification=*params;}
  return result;
}
#if defined(CONFIG_MT_BLE_NATIVE_STORE)
// Use the same real-thread kernel and elapsed clock as the native Settings suite.
inline void k_sleep(unsigned ms){clockOffset+=ms;}
struct bt_addr_le_t {uint8_t bytes[7]{};};
struct bt_bond_info {bt_addr_le_t addr;};
#define BT_ID_DEFAULT 0
inline int bt_addr_le_cmp(const bt_addr_le_t *a,const bt_addr_le_t *b){return memcmp(a,b,sizeof(*a));}
inline const bt_addr_le_t *bt_conn_get_dst(bt_conn *){static const bt_addr_le_t peer{};return &peer;}
inline void bt_id_get(bt_addr_le_t *out,size_t *count){assert(*count==1);*out={};}
inline void bt_foreach_bond(uint8_t,void (*)(const bt_bond_info *,void *),void *){}
inline int bt_conn_set_bondable(bt_conn *,bool){return 0;}
extern "C" int zt_settings_prepare_commit(const char *);
inline int settings_load_subtree(const char *name){return zt_settings_prepare_commit(name);}
#undef BT_CONN_CB_DEFINE
#define BT_CONN_CB_DEFINE(name) static struct {void (*connected)(bt_conn*,uint8_t);void (*disconnected)(bt_conn*,uint8_t);void (*identity_resolved)(bt_conn*,const bt_addr_le_t*,const bt_addr_le_t*);void (*security_changed)(bt_conn*,bt_security_t,bt_security_err);} name
#else
inline void k_sleep(unsigned ms){mockUptime+=ms;}
#endif
