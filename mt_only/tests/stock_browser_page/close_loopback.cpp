// SPDX-License-Identifier: GPL-3.0-only
// Native loopback oracle for the production bounded FIN wait.
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using zsock_pollfd=pollfd;
constexpr int ZSOCK_POLLIN=POLLIN;
static int zsock_poll(zsock_pollfd *fds,nfds_t count,int timeout) { return poll(fds,count,timeout); }
static int64_t k_uptime_get() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#include "../../src/network/browser/PeerClose.inc"

int main(int argc,char **argv) {
    assert(argc==3);
    const bool passive=std::strcmp(argv[1],"passive")==0;
    const int count=std::atoi(argv[2]);
    const int listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    assert(listener>=0);
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    socklen_t len=sizeof(local);
    assert(bind(listener,reinterpret_cast<sockaddr*>(&local),len)==0);
    assert(listen(listener,2)==0);
    assert(getsockname(listener,reinterpret_cast<sockaddr*>(&local),&len)==0);
    std::printf("%d\n",ntohs(local.sin_port));std::fflush(stdout);
    for(int i=0;i<count;i++){
        const int fd=accept(listener,nullptr,nullptr);assert(fd>=0);
        char next=0;
        unsigned match=0;
        constexpr char end[]="\r\n\r\n";
        while(match!=4){assert(recv(fd,&next,1,0)==1);match=next==end[match]?match+1:0;}
        constexpr char response[]="HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        assert(send(fd,response,sizeof(response)-1,0)==ssize_t(sizeof(response)-1));
        if(passive)waitBrowserPeerClose(fd,500);
        assert(close(fd)==0);
    }
    assert(close(listener)==0);
}
