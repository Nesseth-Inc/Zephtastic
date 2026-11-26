// SPDX-License-Identifier: GPL-3.0-only
// Memory-only stand-ins for the actual driver event callback harness. These
// verify event logic, not Zephyr ABI, scheduler, network or hardware behavior.
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IS_ENABLED(x) (x)
#define CONFIG_SNTP 1
#define CONFIG_NET_MGMT_EVENT_INFO 1
#define CONFIG_NET_MGMT_EVENT_STACK_SIZE 4096
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(c,m) _Static_assert(c,m)
#define BIT(x) (1u<<(x))
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...) do{if(0)printf(__VA_ARGS__);}while(0)
#define LOG_WRN(...) do{if(0)printf(__VA_ARGS__);}while(0)
#define LOG_ERR(...) do{if(0)printf(__VA_ARGS__);}while(0)
typedef int32_t atomic_t;typedef int32_t atomic_val_t;
static inline atomic_val_t atomic_get(atomic_t*p){return *p;}
static inline atomic_val_t atomic_set(atomic_t*p,atomic_val_t v){atomic_val_t old=*p;*p=v;return old;}
static inline void atomic_clear(atomic_t*p){*p=0;}
static inline void atomic_inc(atomic_t*p){++*p;}
struct k_event{unsigned bits;};
#define K_EVENT_DEFINE(n) struct k_event n
static inline void k_event_post(struct k_event*p,unsigned bits){p->bits|=bits;}
static inline void k_event_clear(struct k_event*p,unsigned bits){p->bits&=~bits;}
struct k_work{unsigned unused;};
struct k_work_delayable{void(*handler)(struct k_work*);unsigned delay;};
#define K_WORK_DELAYABLE_DEFINE(n,fn) struct k_work_delayable n={fn,0}
#define K_MSEC(n) (n)
#define K_SECONDS(n) ((n)*1000u)
static inline void k_work_reschedule(struct k_work_delayable*p,unsigned delay){p->delay=delay;}
static inline void k_work_schedule(struct k_work_delayable*p,unsigned delay){p->delay=delay;}
#define NET_EVENT_WIFI_CONNECT_RESULT 1u
#define NET_EVENT_WIFI_DISCONNECT_RESULT 2u
#define NET_EVENT_IPV4_DHCP_BOUND 4u
#define NET_REQUEST_WIFI_CONNECT 8u
#define NET_REQUEST_WIFI_DISCONNECT 16u
#define NET_REQUEST_WIFI_PS 32u
#define WIFI_STATUS_CONN_SUCCESS 0
#define WIFI_PS_DISABLED 0
#define WIFI_PS_PARAM_STATE 0
#define WIFI_CHANNEL_ANY 0
#define WIFI_FREQ_BAND_UNKNOWN 0
#define WIFI_MFP_OPTIONAL 0
#define WIFI_SECURITY_TYPE_PSK 1
#define WIFI_SECURITY_TYPE_NONE 0
#define NET_AF_INET 2
#define NET_ADDR_PREFERRED 1
#define NET_IF_MAX_IPV4_ADDR 1
struct in_addr{uint32_t value;};
struct net_if_addr{bool is_used;int addr_state;struct{struct in_addr in_addr;}address;};
struct net_if_ipv4{struct{struct net_if_addr ipv4;}unicast[1];};
struct net_if{struct{struct{struct net_if_ipv4*ipv4;}ip;}config;};
static struct net_if fake_iface;
static inline struct net_if*net_if_get_default(void){return &fake_iface;}
struct wifi_status{int conn_status;};
struct wifi_ps_params{int enabled,type;};
struct wifi_connect_req_params{const uint8_t*ssid;uint8_t ssid_length;int channel,band,mfp;const uint8_t*psk;uint8_t psk_length;int security;};
struct net_mgmt_event_callback{void*info;};
typedef void(*fake_handler)(struct net_mgmt_event_callback*,uint64_t,struct net_if*);
static inline void net_mgmt_init_event_callback(struct net_mgmt_event_callback*c,fake_handler h,uint64_t e){(void)c;(void)h;(void)e;}
static inline void net_mgmt_add_event_callback(struct net_mgmt_event_callback*c){(void)c;}
static inline int net_mgmt(unsigned request,struct net_if*iface,void*params,size_t size){(void)request;(void)iface;(void)params;(void)size;return 0;}
static inline const char *net_addr_ntop(int family,const void *addr,char *buf,size_t size){
 const uint8_t *ip=addr;
 if(family!=NET_AF_INET)return NULL;
 int written=snprintf(buf,size,"%u.%u.%u.%u",ip[0],ip[1],ip[2],ip[3]);
 return written>=0&&(size_t)written<size?buf:NULL;
}
struct sntp_time{uint64_t seconds;};
static const char *expected_ntp="pool.ntp.org";
static unsigned fake_sntp_calls;static int fake_sntp_error;static uint64_t fake_seconds=1700000000;static void(*fake_sntp_hook)(void);
static inline int sntp_simple(const char*server,unsigned timeout,struct sntp_time*time){assert(!strcmp(server,expected_ntp)&&timeout==8000);++fake_sntp_calls;if(fake_sntp_hook)fake_sntp_hook();time->seconds=fake_seconds;return fake_sntp_error;}
