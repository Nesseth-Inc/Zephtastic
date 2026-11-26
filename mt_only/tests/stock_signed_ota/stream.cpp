// SPDX-License-Identifier: GPL-3.0-only
// Exercise update streaming on a Unix socket pair and discovery on IPv4 loopback.
#include "SignedWifiOta.cpp"
#include <vector>
#include <string>
#include <thread>
#include <cassert>

namespace {
void writeAll(int fd,const void *bytes,size_t n){
    const char *p=static_cast<const char*>(bytes);
    while(n){auto k=write(fd,p,n);assert(k>0);p+=k;n-=k;}
}
std::string line(int fd){
    std::string result;char c;while(read(fd,&c,1)==1){result+=c;if(c=='\n')break;assert(result.size()<150);}return result;
}
}
// Use ephemeral local ports so parallel test processes cannot see one another.
void exerciseDiscovery(){
    using namespace mt::update::signedOta;
    auto bindLoopback=[](int fd){
        sockaddr_in endpoint{};endpoint.sin_family=AF_INET;endpoint.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        assert(bind(fd,reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint))==0);
        socklen_t size=sizeof(endpoint);
        assert(getsockname(fd,reinterpret_cast<sockaddr*>(&endpoint),&size)==0);
        return endpoint;
    };
    Fd tcp{socket(AF_INET,SOCK_STREAM,0)}, collector{socket(AF_INET,SOCK_DGRAM,0)};
    const auto station=bindLoopback(tcp.value),destination=bindLoopback(collector.value);
    assert(listen(tcp.value,1)==0);
    Fd androidFd{socket(AF_INET,SOCK_DGRAM,0)},dual{socket(AF_INET,SOCK_DGRAM,0)},fallback{socket(AF_INET,SOCK_DGRAM,0)};
    assert(bindDiscovery(dual.value,station));
    assert(!bindDiscovery(fallback.value,station));
    auto receive=[&](const char *wanted){
        pollfd ready{collector.value,POLLIN,0};
        assert(::poll(&ready,1,1000)==1);
        char message[48];sockaddr_in from{};socklen_t size=sizeof(from);
        const auto n=recvfrom(collector.value,message,sizeof(message),0,reinterpret_cast<sockaddr*>(&from),&size);
        assert(n>=0 && std::string(message,size_t(n))==wanted);
        assert(from.sin_addr.s_addr==station.sin_addr.s_addr);
        return from;
    };
    const char android[]="Meshtastic_1234 1.0.1";
    announceDiscovery(androidFd.value,dual.value,destination,android);
    assert(receive(android).sin_port!=station.sin_port);
    auto apple=receive("MeshtasticOTA");assert(apple.sin_port==station.sin_port);
    Fd application{socket(AF_INET,SOCK_STREAM,0)};
    assert(connect(application.value,reinterpret_cast<sockaddr*>(&apple),sizeof(apple))==0);
    Fd accepted{accept(tcp.value,nullptr,nullptr)};assert(accepted.value>=0);
    announceDiscovery(fallback.value,-1,destination,android);
    assert(receive(android).sin_port!=station.sin_port);
    pollfd ready{collector.value,POLLIN,0};assert(::poll(&ready,1,100)==0);
    announceDiscovery(-1,dual.value,destination,"");
    assert(receive("MeshtasticOTA").sin_port==station.sin_port);
}
void exerciseWire(const std::vector<uint8_t> &good,const uint8_t expected[32],const std::string &cmd){
    exerciseDiscovery();
    int fd[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    std::thread owner([&](){
        bool loaded=mt::update::signedOta::serveClient(fd[1],expected,k_uptime_get()+5000,[](){});
        assert(loaded);close(fd[1]);
    });
    writeAll(fd[0],"VER",3);writeAll(fd[0],"SION\r\n",6);assert(line(fd[0]).starts_with("OK 81 2.8.1+100 "));
    for(char c:cmd)writeAll(fd[0],&c,1);
    writeAll(fd[0],"\n",1);assert(line(fd[0])=="ERASING\n");assert(line(fd[0])=="OK\n");
    // Establish that a partial, already consumed transfer has a progress
    // response while more firmware is still queued at the client.
    writeAll(fd[0],good.data(),501);assert(line(fd[0])=="ACK\n");
    for(size_t i=501;i<good.size();){const size_t n=std::min(size_t(509),good.size()-i);writeAll(fd[0],good.data()+i,n);i+=n;}
    // Stock Android flushes the whole input before starting this read loop.
    unsigned acknowledgements=0;
    for(;;){auto reply=line(fd[0]);if(reply=="OK\n")break;assert(reply=="ACK\n");++acknowledgements;}
    assert(acknowledgements>0);owner.join();close(fd[0]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    std::thread denied([&](){assert(!mt::update::signedOta::serveClient(fd[1],expected,k_uptime_get()+2000,[](){}));close(fd[1]);});
    auto bad=cmd;bad.back()=bad.back()=='0'?'1':'0';bad+='\n';writeAll(fd[0],bad.data(),bad.size());
    assert(line(fd[0]).starts_with("ERR Hash Rejected"));denied.join();close(fd[0]);
}
