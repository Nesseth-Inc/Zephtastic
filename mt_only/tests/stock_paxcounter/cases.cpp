// SPDX-License-Identifier: GPL-3.0-only
#include "fake.h"
#include "modules/peripherals/ZephyrPeripherals.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
namespace oracle {
struct {struct {bool enabled;}paxcounter;}moduleConfig;
struct {struct {bool enabled;}bluetooth;struct {bool wifi_enabled;}network;}config;
#define IRAM_ATTR
enum snifftype_t{MAC_SNIFF_BLE,MAC_SNIFF_WIFI};
unsigned macs_ble,macs_wifi;
std::array<bool,65536> seen{};
int add_to_bucket(uint16_t id){if(seen[id])return 0;seen[id]=true;return 1;}
#include "oracle.h"
}
int main(){
 using namespace mt::modules::peripherals;namespace hw=mt::modules::peripherals::zephyr;
 using Interval=mt::stock::paxcounter::Interval;
 for(bool enabled:{false,true})for(bool bluetooth:{false,true})for(bool wifi:{false,true}){
   oracle::moduleConfig.paxcounter.enabled=enabled;oracle::config.bluetooth.enabled=bluetooth;oracle::config.network.wifi_enabled=wifi;
   assert(oracle::isActive()==(enabled&&Interval::permitted(bluetooth,wifi)));
 }
 assert(Interval::reportPeriod(0,false)==3600&&Interval::reportPeriod(0,true)==43200&&Interval::reportPeriod(2,true)==2);
 PaxcounterCore core;core.configure({true,1,-70,-70},0);
 meshtastic_Paxcount count{};assert(core.reply(0,0,count)&&!count.ble&&!count.wifi);
 testBleRunning=true;assert(!hw::startBleObserver(true)&&testBleRunning&&!testBleCb);
 hw::stopBleObserver();assert(testBleRunning);testBleRunning=false;assert(hw::startBleObserver(true));
 testMode=STA;hw::startWifiObserver(true);assert(!testMonitor&&!testCallback&&testChannel==9);testMode=ESP32_WIFI_MODE_NULL;
 testMonitor=true;hw::startWifiObserver(true);assert(testMonitor&&!testCallback&&testChannel==9);testMonitor=false;
 for(int injected=1;injected<=4;++injected){testFailure=injected;hw::startWifiObserver(true);testFailure=0;
    hw::stopWifiObserver();assert(!testMonitor&&!testCallback&&testChannel==9&&testFilter.filter_mask==15);}
 hw::startWifiObserver(true);assert(testMonitor&&testCallback&&testChannel==3&&testFilter.filter_mask==3);hw::capturePax(true);
 wifi_promiscuous_pkt_t packet{};packet.rx_ctrl={24,-70};
 for(unsigned k=0;k<256;++k){
   alignas(uint16_t) uint8_t address[6]={uint8_t(k%4),17,0,1,uint8_t(k),45};
   memcpy(packet.payload+10,address,6);testNow=k;
   testCallback(&packet,k%2?WIFI_PKT_MGMT:WIFI_PKT_DATA);
   oracle::mac_add(address,oracle::MAC_SNIFF_WIFI);hw::drainPaxObservations(core);
   bt_addr_le_t little{};for(size_t i=0;i<6;++i)little.a.val[5-i]=address[i];
   bt_le_scan_recv_info info{&little,-70};testBleCb->recv(&info,nullptr);hw::drainPaxObservations(core);
   oracle::mac_add(address,oracle::MAC_SNIFF_BLE);
 }
 // Control and truncated frames, and a weak source, do not appear in the report.
 packet.payload[10]=2;packet.payload[14]=0xff;packet.payload[15]=0xff;testCallback(&packet,WIFI_PKT_CTRL);
 packet.rx_ctrl.sig_len=14;testCallback(&packet,WIFI_PKT_MGMT);packet.rx_ctrl={24,-71};testCallback(&packet,WIFI_PKT_DATA);
 bt_addr_le_t addr{{{255,255,0,0,0,2}}};bt_le_scan_recv_info info{&addr,-70};testBleCb->recv(&info,nullptr);
 alignas(uint16_t) uint8_t last[]={2,0,0,0,255,255};oracle::mac_add(last,oracle::MAC_SNIFF_BLE);
 hw::drainPaxObservations(core);
 assert(core.reply(499,0,count)&&!count.wifi&&!count.ble);
 testNow=499;hw::startWifiObserver(true);assert(testChannel==3);
 for(auto expected:{4,5,3}){testNow+=500;hw::startWifiObserver(true);assert(testChannel==expected);}
 core.publishIntervals(1000);assert(core.reply(1000,1,count)&&count.wifi==oracle::macs_wifi&&count.ble==oracle::macs_ble&&count.uptime==1);
 unsigned calls=0;core.sendInfo(1000,1,[&](auto&){++calls;return false;});
 core.sendInfo(1001,1,[&](auto&){++calls;return true;});assert(calls==1);
 core.publishIntervals(2000);assert(core.reply(2000,2,count)&&!count.ble&&!count.wifi);
 core.sendInfo(2000,2,[&](auto&){++calls;return true;});assert(calls==2);
 hw::capturePax(false);hw::stopWifiObserver();hw::stopBleObserver();
 assert(!testMonitor&&!testCallback&&!testBleCb&&!testBleRunning&&testChannel==9&&testFilter.filter_mask==15);
 core.configure({false,1,0,0},2001);assert(!core.reply(2001,2,count));
 puts("PASS stock exclusivity, shared 16-bit bucket, empty & populated report/reset, controlled BLE/WiFi callbacks, four partial-start failures, legal channels, channel/filter restoration");
}
