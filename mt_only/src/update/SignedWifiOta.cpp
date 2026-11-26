// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic unified OTA's station TCP/UDP port and wire responses. The owner
// must authorize exactly one digest before either socket can be created.
#include "SignedOta.h"
#include "ZephyrWiFiStation.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <string.h>
namespace mt::update::signedOta {
namespace {
struct Fd {
    int value=-1;
    ~Fd(){if(value>=0)zsock_close(value);}
};
bool bindDiscovery(int fd,const sockaddr_in &station){
    // The Apple client reuses the beacon's complete source endpoint for TCP.
    // Android takes its source address and connects to its own fixed OTA port.
    return zsock_bind(fd,reinterpret_cast<const sockaddr*>(&station),sizeof(station))==0;
}
void announceDiscovery(int androidFd,int appleFd,const sockaddr_in &destination,const char *android){
    const auto *to=reinterpret_cast<const sockaddr*>(&destination);
    if(androidFd>=0&&*android)(void)zsock_sendto(androidFd,android,strlen(android),ZSOCK_MSG_DONTWAIT,to,sizeof(destination));
    if(appleFd>=0){
        constexpr char apple[]="MeshtasticOTA";
        (void)zsock_sendto(appleFd,apple,sizeof(apple)-1,ZSOCK_MSG_DONTWAIT,to,sizeof(destination));
    }
}
bool pending(){return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;}
bool sendText(int fd,const char *text,void (*progress)()){
    size_t left=strlen(text);const int64_t deadline=k_uptime_get()+2000;
    while(left&&k_uptime_get()<deadline){
        const int n=zsock_send(fd,text,left,ZSOCK_MSG_DONTWAIT);
        if(n>0){left-=n;text+=n;}else if(n==0||!pending())return false;
        if(left){progress();k_sleep(K_MSEC(5));}
    }
    return !left;
}
bool serveClient(int fd,const uint8_t expected[32],int64_t limit,void (*progress)()){
    Session session(expected);uint8_t bytes[1024];char line[96];size_t used=0;
    int64_t idle=k_uptime_get()+15000;
    while(k_uptime_get()<idle&&k_uptime_get()<limit) {
        progress();int n=zsock_recv(fd,bytes,sizeof(bytes),ZSOCK_MSG_DONTWAIT);
        if(n==0)return false;
        if(n<0){if(!pending())return false;k_sleep(K_MSEC(5));continue;}
        idle=k_uptime_get()+15000;
        size_t i=0;
        while(i<size_t(n)){
            if(session.downloading()){
                const char *reply=session.write(bytes+i,n-i);
                // Android streams without waiting, then reads ACK lines until the
                // final OK. Each ACK starts a fresh 10 s final-response deadline.
                // The stock loader sends it after each accepted network chunk.
                const bool sent=sendText(fd,reply,progress);
                if(session.complete())return true;
                if(!sent||strcmp(reply,"ACK\n"))return false;
                break;
            }
            uint8_t c=bytes[i++];
            if(c=='\r')continue;
            if(c=='\n'&&used){
                line[used]='\0';used=0;
                const char *reply=session.command(line);
                if(!sendText(fd,reply,progress)||!strncmp(reply,"ERR",3))return false;
            }else if(c!='\n'){
                if(c<0x20||c>0x7e||used==sizeof(line)-1)return false;
                line[used++]=c;
            }
        }
    }
    return false;
}
}
void serveWifi(const uint8_t expected[32],void (*progress)()){
    char address[NET_IPV4_ADDR_LEN];if(!zc_wifi_station_ip(address,sizeof(address)))return;
    sockaddr_in station{};station.sin_family=AF_INET;station.sin_port=htons(3232);
    if(zsock_inet_pton(AF_INET,address,&station.sin_addr)!=1)return;
    Fd listener;listener.value=zsock_socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(listener.value<0||zsock_bind(listener.value,reinterpret_cast<sockaddr*>(&station),sizeof(station))||
       zsock_listen(listener.value,1))return;
    Fd discovery;discovery.value=zsock_socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    // Apple cancels a UDP flow after its first datagram, even on a rejected name.
    // Give it an independent flow; the stock Android announcement remains ephemeral.
    Fd apple;apple.value=zsock_socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    const int appleFd=apple.value>=0&&bindDiscovery(apple.value,station)?apple.value:-1;
    char beacon[32]{};const net_if *iface=net_if_get_default();
    if(discovery.value>=0&&iface){const auto *link=net_if_get_link_addr(const_cast<net_if*>(iface));
        if(link&&link->len==6)snprintk(beacon,sizeof(beacon),"Meshtastic_%02X%02X 1.0.1",link->addr[4],link->addr[5]);}
    sockaddr_in broadcast=station;broadcast.sin_addr.s_addr=htonl(0xffffffffu);
    const int64_t end=k_uptime_get()+240000;int64_t nextBeacon=0;
    while(k_uptime_get()<end){
        progress();
        if(k_uptime_get()>=nextBeacon){
            nextBeacon=k_uptime_get()+1000;
            announceDiscovery(discovery.value,appleFd,broadcast,beacon);
        }
        zsock_pollfd poll{listener.value,ZSOCK_POLLIN,0};
        if(zsock_poll(&poll,1,50)<=0)continue;
        if(!(poll.revents&ZSOCK_POLLIN))break;
        Fd client;client.value=zsock_accept(listener.value,nullptr,nullptr);
        if(client.value>=0&&serveClient(client.value,expected,end,progress)){k_sleep(K_MSEC(150));return;}
    }
}
}
