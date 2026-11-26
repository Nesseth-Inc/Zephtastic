// SPDX-License-Identifier: GPL-3.0-only
#include "wifi_native_api.h"
#include "../../zephcore/adapters/wifi/ZephyrWiFiStation.c"
static unsigned callbacks;static uint32_t received;
static void synced(uint32_t value){++callbacks;received=value;}
static struct wifi_status good={WIFI_STATUS_CONN_SUCCESS};
static void reset_fixture(void){
 expected_ntp="pool.ntp.org";callbacks=received=fake_sntp_calls=0;fake_sntp_error=0;fake_seconds=1700000000;fake_sntp_hook=NULL;
 zc_wifi_station_start("A","B",synced);wifi_cb.info=&good;g_wifi_events.bits=0;
 assert(!zc_wifi_station_is_connected()&&connect_work.delay==500);
}
static void associated(void){wifi_event_handler(&wifi_cb,NET_EVENT_WIFI_CONNECT_RESULT,&fake_iface);}
static void lease(void){ipv4_event_handler(&ipv4_cb,NET_EVENT_IPV4_DHCP_BOUND,&fake_iface);}
static void disconnected(void){wifi_event_handler(&wifi_cb,NET_EVENT_WIFI_DISCONNECT_RESULT,&fake_iface);}
static void during_sntp(void){
 char ip[16];
 assert(zc_wifi_station_ip(ip,sizeof(ip))&&!strcmp(ip,"192.0.2.17"));
 assert(!zc_wifi_station_is_connected()&&!(g_wifi_events.bits&WIFI_READY_BIT));
}
int main(void){
 reset_fixture();lease();assert(!callbacks&&!zc_wifi_station_is_connected());associated();assert(callbacks==1&&received==1700000000&&zc_wifi_station_is_connected());
 puts("PASS pinned ESP32 DHCP-before-CONNECT order synchronizes after both observations");
 reset_fixture();associated();assert(!callbacks&&!zc_wifi_station_is_connected());lease();assert(callbacks==1&&zc_wifi_station_is_connected());associated();assert(fake_sntp_calls==1&&callbacks==1);
 puts("PASS association-before-DHCP and duplicate CONNECT keep one first sync");
 reset_fixture();fake_sntp_error=-1;associated();lease();assert(!callbacks&&fake_sntp_calls==1&&zc_wifi_station_is_connected());
 puts("PASS failed SNTP permits IP readiness without inventing UTC success");
 reset_fixture();fake_sntp_hook=disconnected;associated();lease();assert(!callbacks&&!zc_wifi_station_is_connected()&&!g_wifi_events.bits&&connect_work.delay==5000);
 puts("PASS disconnect during SNTP suppresses stale readiness and UTC callback");
 reset_fixture();associated();lease();disconnected();assert(!zc_wifi_station_is_connected()&&!atomic_get(&s_dhcp_bound));associated();assert(!zc_wifi_station_is_connected());lease();assert(zc_wifi_station_is_connected());
 puts("PASS reconnect requires a fresh lease and does not reuse old ready state");
 reset_fixture();fake_seconds=UINT64_C(0x100000000);associated();lease();assert(!callbacks&&zc_wifi_station_is_connected());reset_fixture();wifi_cb.info=NULL;associated();assert(!zc_wifi_station_is_connected()&&!callbacks&&connect_work.delay==10000);
 puts("PASS out-of-range UTC and missing success info never synthesize observations");
 reset_fixture();expected_ntp="192.0.2.1";zc_wifi_station_start_ntp("A","B",expected_ntp,synced);
 associated();lease();assert(callbacks==1&&received==1700000000&&fake_sntp_calls==1);
 puts("PASS owner-supplied server reaches SNTP and reports its verified time");
 reset_fixture();
 struct net_if_ipv4 ipv4={0};unsigned char address[4]={192,0,2,17};
 ipv4.unicast[0].ipv4.is_used=true;ipv4.unicast[0].ipv4.addr_state=NET_ADDR_PREFERRED;
 memcpy(&ipv4.unicast[0].ipv4.address.in_addr,address,sizeof(address));fake_iface.config.ip.ipv4=&ipv4;
 fake_sntp_hook=during_sntp;lease();associated();assert(zc_wifi_station_is_connected());
 disconnected();char ip[16];assert(!zc_wifi_station_ip(ip,sizeof(ip)));
 puts("PASS preferred lease exists during SNTP and cannot survive disconnect");
 return 0;
}
