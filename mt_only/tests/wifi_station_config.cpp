// SPDX-License-Identifier: GPL-3.0-only
#include "../src/network/WifiStationConfigMapping.h"
#include "../src/network/WifiStationConfigExport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using S=mt::network::WifiStationSettings;
using M=mt::network::WifiStationConfigMapping;
using E=mt::network::WifiStationConfigExport;
using N=meshtastic_Config_NetworkConfig;
static bool zero(const void*p,size_t n){auto b=static_cast<const unsigned char*>(p);for(size_t i=0;i<n;++i)if(b[i])return false;return true;}
static void refused(const N&n,const S&current,bool capable=true){S out;assert(out.set(false,"X",1,"Y",1));S before=out;assert(!M::fromConfig(n,capable,current,out));assert(!memcmp(out.bytes,before.bytes,S::Size));}
int main(){
 S current,out;assert(current.set(true,"A",1,"B",1));N n=meshtastic_Config_NetworkConfig_init_zero;n.wifi_enabled=true;strcpy(n.wifi_ssid,"C");strcpy(n.wifi_psk,"D");
 assert(M::fromConfig(n,true,current,out)&&out.valid()&&out.enabled()&&out.bytes[8]=='C'&&out.bytes[41]=='D');refused(n,current,false);
 n.wifi_enabled=false;strcpy(n.wifi_ssid,"A");strcpy(n.wifi_psk,"sekrit");assert(M::fromConfig(n,false,current,out)&&!out.enabled()&&out.ssidLength()==1&&out.pskLength()==1&&out.bytes[41]=='B');
 N empty=meshtastic_Config_NetworkConfig_init_zero;assert(M::fromConfig(empty,false,current,out)&&!out.enabled()&&out.ssidLength()==0&&out.pskLength()==0);
 strcpy(empty.wifi_psk,"sekrit");assert(M::fromConfig(empty,false,current,out)&&!out.enabled()&&out.ssidLength()==0&&out.pskLength()==1&&out.bytes[41]=='B');
 N reset=meshtastic_Config_NetworkConfig_init_zero; n=reset;
 n.wifi_enabled=true;strcpy(n.wifi_ssid,"A");strcpy(n.wifi_psk,"B");
 for(unsigned k=0;k<11;++k){N bad=n;switch(k){case 0:bad.ntp_server[0]='/';break;case 1:bad.eth_enabled=true;break;case 2:memset(&bad.address_mode,0xff,sizeof(bad.address_mode));break;case 3:bad.has_ipv4_config=true;break;case 4:bad.ipv4_config.ip=1;break;case 5:bad.ipv4_config.gateway=1;break;case 6:bad.ipv4_config.subnet=1;break;case 7:bad.ipv4_config.dns=1;break;case 8:bad.rsyslog_server[0]='A';break;case 9:bad.enabled_protocols=1;break;case 10:bad.ipv6_enabled=true;break;}refused(bad,current);}
 N bad=n;bad.wifi_ssid[2]='X';refused(bad,current);bad=n;memset(bad.wifi_ssid,'X',sizeof(bad.wifi_ssid));refused(bad,current);bad=n;memset(bad.wifi_psk,'Y',sizeof(bad.wifi_psk));refused(bad,current);
 memset(n.wifi_ssid,'S',32);n.wifi_ssid[32]=0;memset(n.wifi_psk,'P',64);n.wifi_psk[64]=0;assert(M::fromConfig(n,true,current,out)&&out.ssidLength()==32&&out.pskLength()==64);
 N exported=meshtastic_Config_NetworkConfig_init_zero;assert(E::toConfig(out,exported)&&exported.wifi_enabled&&strlen(exported.wifi_ssid)==32&&strlen(exported.wifi_psk)==64);assert(zero(exported.ntp_server,sizeof(exported.ntp_server))&&!exported.eth_enabled&&exported.address_mode==meshtastic_Config_NetworkConfig_AddressMode_DHCP&&!exported.has_ipv4_config&&zero(&exported.ipv4_config,sizeof(exported.ipv4_config))&&zero(exported.rsyslog_server,sizeof(exported.rsyslog_server))&&exported.enabled_protocols==0&&!exported.ipv6_enabled);
 N saved=n;
 strcpy(saved.ntp_server,"time.example");
 assert(M::fromConfig(saved,true,current,out));
 assert(!strcmp(out.ntpServer(),"time.example") && E::toConfig(out,exported));
 assert(!strcmp(exported.ntp_server,saved.ntp_server));
 saved.wifi_enabled=false; refused(saved,current,false);
 saved.ntp_server[sizeof(saved.ntp_server)-1]='x';refused(saved,current);
 S invalid=current;invalid.bytes[106]^=1;refused(n,invalid);unsigned char snapshot[sizeof(exported)];memcpy(snapshot,&exported,sizeof(exported));assert(!E::toConfig(invalid,exported)&&!memcmp(snapshot,&exported,sizeof(exported)));
 puts("PASS_WIFI_CONFIG_BOARD_GUARD_STOCK_PLACEHOLDER_EXPLICIT_CLEAR_BOUNDS_ENUM_REFUSALS_AND_LOCAL_EXPORT");
}
