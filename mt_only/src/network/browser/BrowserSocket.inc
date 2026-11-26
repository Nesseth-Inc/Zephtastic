// SPDX-License-Identifier: GPL-3.0-only
// Included in the TCP transport's namespace so both protocols use one owner,
// generation, RX queue, TX queue and socket thread.
namespace web = mt::network::browser;
static web::HttpMessaging http __attribute__((section(".ext_ram.bss.mt_browser")));
static uint32_t stationAddress() {
    auto *iface=net_if_get_wifi_sta();
    if(!iface || !net_if_is_up(iface) || !zc_wifi_station_is_connected())return 0;
    auto *address=net_if_ipv4_get_global_addr(iface,NET_ADDR_PREFERRED);
    return address?address->s_addr:0;
}
static bool localPeer(uint32_t local,uint32_t remote) {
    if(!local||!remote||stationAddress()!=local)return false;
    net_in_addr peer{};peer.s_addr=remote;
    auto *iface=net_if_get_wifi_sta();
    return iface&&net_if_ipv4_addr_mask_cmp(iface,&peer);
}
struct BrowserSocket {
    int fd;
    int64_t deadline=k_uptime_get()+4500;
    int read(void *out,size_t cap,int64_t until) {
        for(;;){
            int64_t left=until-k_uptime_get();if(left<=0)return 0;
            zsock_pollfd poll{};poll.fd=fd;poll.events=ZSOCK_POLLIN;
            int ready=zsock_poll(&poll,1,int(left));
            if(ready<0&&errno==EINTR)continue;
            if(ready<=0)return 0;
            auto n=zsock_recv(fd,out,cap,ZSOCK_MSG_DONTWAIT);
            if(n<0 && wouldBlock())continue;
            return int(n);
        }
    }
    bool write(const void *p,size_t n) {
        auto *bytes=static_cast<const uint8_t *>(p);
        while(n){
            int64_t left=deadline-k_uptime_get();if(left<=0)return false;
            auto sent=zsock_send(fd,bytes,n,ZSOCK_MSG_DONTWAIT);
            if(sent>0){bytes+=sent;n-=size_t(sent);continue;}
            if(!sent||!wouldBlock())return false;
            zsock_pollfd poll{};poll.fd=fd;poll.events=ZSOCK_POLLOUT;
            int ready=zsock_poll(&poll,1,int(left));
            if(ready<0&&errno==EINTR)continue;
            if(ready<=0)return false;
        }
        return true;
    }
};
struct BrowserBackend {
    uint32_t local,peer;
    int64_t now()const{return k_uptime_get();}
    bool stillLocal()const{return localPeer(local,peer);}
    bool random(uint8_t *out,size_t n)const{return sys_csrand_get(out,n)==0;}
    uint32_t open()const{return openSession();}
    void close(uint32_t token)const {
        if(token && token==generation)closeSession(); // generation changes on this thread only
    }
    bool queue(uint32_t token,const uint8_t *bytes,size_t n)const {
        if(!n||n>sizeof(Packet::bytes))return false;
        Packet packet;packet.session=token;packet.length=n;memcpy(packet.bytes,bytes,n);
        k_mutex_lock(&stateLock,K_FOREVER);
        bool ok=link&&generation==token && !k_msgq_put(&incoming,&packet,K_NO_WAIT);
        k_mutex_unlock(&stateLock);return ok;
    }
    size_t available(uint32_t token)const {
        k_mutex_lock(&stateLock,K_FOREVER);
        size_t n=link&&generation==token?outgoing.size():0;
        k_mutex_unlock(&stateLock);return n;
    }
    bool pop(uint32_t token,uint8_t *dest,size_t cap,size_t &size)const {
        FixedTxQueue<64>::Frame frame;
        k_mutex_lock(&stateLock,K_FOREVER);
        bool ok=link&&generation==token&&outgoing.pop(frame);
        k_mutex_unlock(&stateLock);
        if(!ok||frame.length<5||size_t(frame.length-4)>cap)return false;
        size=frame.length-4;memcpy(dest,frame.bytes+4,size);return true;
    }
};
static int bindBrowser(uint32_t ip) {
    net_sockaddr_in addr{};addr.sin_family=NET_AF_INET;addr.sin_addr.s_addr=ip;
    addr.sin_port=net_htons(80);
    int fd=zsock_socket(NET_AF_INET,NET_SOCK_STREAM,NET_IPPROTO_TCP);
    if(fd<0)return -1;
    int one=1;
    (void)zsock_setsockopt(fd,ZSOCK_SOL_SOCKET,ZSOCK_SO_REUSEADDR,&one,sizeof(one));
    if(zsock_bind(fd,reinterpret_cast<net_sockaddr*>(&addr),sizeof(addr)) ||
       zsock_listen(fd,2) || zsock_fcntl(fd,ZVFS_F_SETFL,ZVFS_O_NONBLOCK)<0){
        zsock_close(fd);return -1;
    }
    return fd;
}
// A successful Content-Length or chunked response tells the peer exactly when
// it can close. Let it send FIN first so our few target-side TCP slots do not
// sit in TIME_WAIT after each ordinary request. A stalled peer has a deadline.
#include "PeerClose.inc"
static void rejectBusyBrowser(int listener) {
    int fd=zsock_accept(listener,nullptr,nullptr);if(fd<0)return;
    BrowserSocket io{fd};io.deadline=k_uptime_get()+30;
    web::HttpMessaging::fail(io,409);waitBrowserPeerClose(fd,15);zsock_close(fd);
}
static void serve(int,uint32_t,int);
static void runListeners(int raw) {
    int webfd=-1;uint32_t local=0;int64_t retry=0;
    for(;;){
        const uint32_t ip=stationAddress();
        BrowserBackend lease{local,http.session.peer};http.expire(lease);
        if(ip!=local){
            if(webfd>=0)zsock_close(webfd);
            webfd=-1;local=ip;retry=0;
        }
        if(local && webfd<0 && k_uptime_get()>=retry){webfd=bindBrowser(local);retry=k_uptime_get()+2000;}
        zsock_pollfd p[2]{};p[0].fd=raw;p[1].fd=webfd;p[0].events=p[1].events=ZSOCK_POLLIN;
        int ready=zsock_poll(p,webfd<0?1:2,200);
        if(ready<0){if(errno==EINTR)continue;break;}
        if(p[0].revents&ZSOCK_POLLIN){
            int fd=zsock_accept(raw,nullptr,nullptr);
            if(fd>=0){
                if(!http.session.generation){auto token=openSession();serve(fd,token,webfd);closeSession();}
                zsock_close(fd);
            }
        }
        if(webfd<0||!(p[1].revents&ZSOCK_POLLIN))continue;
        net_sockaddr_in peer{},ours{};net_socklen_t length=sizeof(peer);
        int fd=zsock_accept(webfd,reinterpret_cast<net_sockaddr*>(&peer),&length);if(fd<0)continue;
        BrowserSocket socket{fd};BrowserBackend backend{local,peer.sin_addr.s_addr};
        length=sizeof(ours);char address[NET_IPV4_ADDR_LEN];
        if(zsock_getsockname(fd,reinterpret_cast<net_sockaddr*>(&ours),&length)==0 &&
           ours.sin_addr.s_addr==local && backend.stillLocal() &&
           net_addr_ntop(NET_AF_INET,&ours.sin_addr,address,sizeof(address))){
            http.handle(socket,backend,peer.sin_addr.s_addr,address,
                        {web::MessagingPage,sizeof(web::MessagingPage)-1,web::MessagingCsp});
        }else web::HttpMessaging::fail(socket,403);
        waitBrowserPeerClose(fd,500);
        zsock_close(fd);
    }
    if(webfd>=0)zsock_close(webfd);
    http.session.clear();
}
