/* SPDX-License-Identifier: MIT
 * Adapted from ZephCore TcpCompanionTransport.c (MIT). Socket setup, single
 * client lifetime, partial-send handling and bounded congestion retain that
 * design. Meshtastic framing, session queues and separate IO ownership replace
 * MeshCore-specific wire framing/workqueue/classification. See ZEPHCORE-REUSE.
 */
#include "TcpTransport.h"
#if defined(CONFIG_MT_TCP_COMPANION)
#include "MtTcpFrameParser.h"
#include "MtTcpTxQueue.h"
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#if defined(CONFIG_MT_BROWSER_MESSAGING)
#include "browser/HttpMessaging.h"
#include "browser/MessagingPage.h"
#include "ZephyrWiFiStation.h"
#include <zephyr/net/net_if.h>
#include <zephyr/random/random.h>
#endif
#include <errno.h>
#include <string.h>
namespace mt::tcp {
K_MUTEX_DEFINE(stateLock);
K_MSGQ_DEFINE(incoming,sizeof(Packet),4,4);
// ZephCore's conditional PSRAM placement pattern. Only thread-owned queued
// frame bytes move: pop copies into an internal ioStack Frame before socket IO.
// The SDK initializes/zeroes external BSS before C++ constructors. Service,
// radio/DMA buffers, mutex, message queue and socket thread stack stay internal.
BUILD_ASSERT(IS_ENABLED(CONFIG_ESP_SPIRAM));
BUILD_ASSERT(DT_PROP(DT_NODELABEL(psram0),size)==8*1024*1024);
static FixedTxQueue<64> outgoing __attribute__((section(".ext_ram.bss.mt_tcp_tx")));
static bool link=false,started=false;
static uint32_t generation=0;
static K_KERNEL_STACK_DEFINE(ioStack,8192);
static struct k_thread ioThread;
static void advance(){if(!++generation)++generation;}
static uint32_t openSession(){
 k_mutex_lock(&stateLock,K_FOREVER);advance();outgoing.reset();k_msgq_purge(&incoming);link=true;
 uint32_t token=generation;k_mutex_unlock(&stateLock);return token;
}
static void closeSession(){
 k_mutex_lock(&stateLock,K_FOREVER);link=false;advance();outgoing.reset();k_msgq_purge(&incoming);k_mutex_unlock(&stateLock);
}
static bool wouldBlock(){return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;}
// One absolute two-second deadline covers every partial send, rather than
// extending a timeout indefinitely whenever a wedged peer accepts one byte.
static bool sendAll(int fd,const uint8_t *bytes,size_t size){
 const int64_t deadline=k_uptime_get()+2000;size_t sent=0;
 while(sent<size){
  if(k_uptime_get()>=deadline)return false;
  ssize_t n=zsock_send(fd,bytes+sent,size-sent,ZSOCK_MSG_DONTWAIT);
  if(n>0){sent+=size_t(n);continue;}if(!n||!wouldBlock())return false;
  struct zsock_pollfd p{};p.fd=fd;p.events=ZSOCK_POLLOUT;
  if(zsock_poll(&p,1,20)<0&&errno!=EINTR)return false;
 }
 return true;
}
#if defined(CONFIG_MT_BROWSER_MESSAGING)
#include "browser/BrowserSocket.inc"
#endif
static void serve(int fd,uint32_t token
#if defined(CONFIG_MT_BROWSER_MESSAGING)
                  ,int web
#endif
                  ){

 FrameParser parser;parser.reset(token);uint8_t bytes[128];Frame body;
 for(;;){
#if defined(CONFIG_MT_BROWSER_MESSAGING)
  if(web>=0)rejectBusyBrowser(web);
#endif
  FixedTxQueue<64>::Frame output;
  k_mutex_lock(&stateLock,K_FOREVER);bool has=outgoing.pop(output);k_mutex_unlock(&stateLock);
  if(has&&!sendAll(fd,output.bytes,output.length))return;
  ssize_t n=zsock_recv(fd,bytes,sizeof(bytes),ZSOCK_MSG_DONTWAIT);
  if(n==0)return;
  if(n<0){if(!wouldBlock())return;k_sleep(K_MSEC(5));continue;}
  for(ssize_t i=0;i<n;++i){
   auto result=parser.push(bytes[i],k_uptime_get_32(),token,body);
   if(result==FrameParser::Result::Invalid)return;
   if(result!=FrameParser::Result::FrameReady)continue;
   Packet packet;packet.session=token;packet.length=body.length;memcpy(packet.bytes,body.payload,body.length);
   // Pause socket reads for flow control. If the owner cannot drain accepted
   // frames within 250ms, terminate this epoch; never drop and continue.
   if(k_msgq_put(&incoming,&packet,K_MSEC(250)))return;
  }
 }
}
static void io(void*,void*,void*){
 struct net_sockaddr_in addr{};addr.sin_family=NET_AF_INET;addr.sin_addr.s_addr=0;addr.sin_port=net_htons(4403);
 int listener=zsock_socket(NET_AF_INET,NET_SOCK_STREAM,NET_IPPROTO_TCP);if(listener<0)return;
 int one=1;(void)zsock_setsockopt(listener,ZSOCK_SOL_SOCKET,ZSOCK_SO_REUSEADDR,&one,sizeof(one));
 if(zsock_bind(listener,reinterpret_cast<struct net_sockaddr*>(&addr),sizeof(addr))<0||
    zsock_listen(listener,1)<0||zsock_fcntl(listener,ZVFS_F_SETFL,ZVFS_O_NONBLOCK)<0){zsock_close(listener);return;}
#if defined(CONFIG_MT_BROWSER_MESSAGING)
 runListeners(listener);
#else
 for(;;){
  struct zsock_pollfd p{};p.fd=listener;p.events=ZSOCK_POLLIN;
  int available=zsock_poll(&p,1,200);if(available<0){if(errno==EINTR)continue;break;}if(!available)continue;
  struct net_sockaddr_in peer{};net_socklen_t length=sizeof(peer);
  int fd=zsock_accept(listener,reinterpret_cast<struct net_sockaddr*>(&peer),&length);
  if(fd<0){if(wouldBlock())continue;break;}
  uint32_t token=openSession();serve(fd,token);closeSession();zsock_close(fd);
 }
#endif
 closeSession();zsock_close(listener);
}
void init(bool enabled){
 if(!enabled||started)return;
 started=true;
 k_thread_create(&ioThread,ioStack,K_KERNEL_STACK_SIZEOF(ioStack),io,nullptr,nullptr,nullptr,K_PRIO_PREEMPT(8),0,K_NO_WAIT);
 k_thread_name_set(&ioThread,"mt_tcp");
}
bool connected(){k_mutex_lock(&stateLock,K_FOREVER);bool value=link;k_mutex_unlock(&stateLock);return value;}
uint32_t session(){k_mutex_lock(&stateLock,K_FOREVER);uint32_t value=generation;k_mutex_unlock(&stateLock);return value;}
bool receive(Packet &packet){return k_msgq_get(&incoming,&packet,K_NO_WAIT)==0;}
bool dispatch(const Packet &p,void(*callback)(const Packet &,void *),void *context){
 k_mutex_lock(&stateLock,K_FOREVER);bool valid=callback&&link&&p.session==generation&&p.length&&p.length<=512;
 if(valid)callback(p,context);
 k_mutex_unlock(&stateLock);return valid;
}
bool send(const uint8_t *bytes,size_t size,uint32_t token,bool idleOnly){
 k_mutex_lock(&stateLock,K_FOREVER);bool ok=link&&generation==token&&outgoing.pushFramed(bytes,size,idleOnly);k_mutex_unlock(&stateLock);return ok;
}
}
#else
namespace mt::tcp {
void init(bool){}bool connected(){return false;}uint32_t session(){return 0;}
bool receive(Packet&){return false;}
bool dispatch(const Packet&,void(*)(const Packet&,void*),void*){return false;}
bool send(const uint8_t*,size_t,uint32_t,bool){return false;}
}
#endif
