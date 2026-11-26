// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "network/browser/HttpMessaging.h"
#include "network/browser/MessagingPage.h"
#include "network/MtTcpTxQueue.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
namespace web=mt::network::browser;
using std::string;
using Frame=mt::tcp::FixedTxQueue<64>::Frame;
struct RadioHost:Host {
    mt::tcp::FixedTxQueue<64> frames;
    unsigned saves=0,secrets=0;
    explicit RadioHost(mt::Service &s):Host(s){}
    bool networkConfig(meshtastic_Config_NetworkConfig &out)const override{
        out={};strcpy(out.wifi_psk,"fixture-protected-network-setting");return true;
    }
    bool send(const uint8_t *p,size_t n)override {
        meshtastic_FromRadio f{};auto in=pb_istream_from_buffer(p+4,n-4);
        assert(pb_decode(&in,meshtastic_FromRadio_fields,&f));
        if(f.which_payload_variant==meshtastic_FromRadio_config_tag&&f.config.which_payload_variant==meshtastic_Config_network_tag &&
           !strcmp(f.config.payload_variant.network.wifi_psk,"fixture-protected-network-setting"))++secrets;
        return frames.pushFramed(p,n);
    }
    bool sendIfQueueEmpty(const uint8_t *p,size_t n)override{return !frames.size()&&send(p,n);}
    int save(const mt::Config &)override{++saves;return -1;}
};
struct Backend{
    Fixture fixture;RadioHost host{fixture.s};mt::companion::CompanionWorkspace workspace;
    mt::Companion companion{fixture.s,host,81,workspace,2};
    unsigned generation=0,opens=0,closes=0,accepted=0;
    int64_t clock=100;
    bool local=true,entropy=true,capacity=true,raw=false;
    int64_t now()const{return clock;}
    bool stillLocal()const{return local;}
    bool random(uint8_t *p,size_t n)const{if(!entropy)return false;for(size_t i=0;i<n;i++)p[i]=uint8_t(1+i+opens);return true;}
    uint32_t open(){if(raw)return 0;++opens;host.frames.reset();companion.resetStream();return ++generation;}
    void close(uint32_t token){assert(token==generation);++closes;++generation;host.frames.reset();companion.resetStream();}
    bool queue(uint32_t token,const uint8_t *data,size_t n){
        assert(token==generation && !raw);if(!capacity)return false;++accepted;
        companion.packet(data,n,uint32_t(clock),mt::Companion::WorkspaceProof{true});return true;
    }
    size_t available(uint32_t token){assert(token==generation);return host.frames.size();}
    bool pop(uint32_t token,uint8_t *data,size_t cap,size_t &n){
        assert(token==generation);Frame f;if(!host.frames.pop(f))return false;
        n=f.length-4;assert(n<=cap);memcpy(data,f.bytes+4,n);return true;
    }
};
struct Socket {
    int fd;size_t fragment;
    int read(void *p,size_t n,int64_t){return int(recv(fd,p,std::min(n,fragment),0));}
    bool write(const void *p,size_t n){
      const char *src=static_cast<const char *>(p);
      while(n){auto count=send(fd,src,n,0);if(count<=0)return false;n-=size_t(count);src+=count;}return true;
    }
};
struct Response{
    int code;string headers,body;
    explicit Response(const string &raw){
        auto offset=raw.find("\r\n\r\n");assert(offset!=string::npos);
        headers=raw.substr(0,offset);body=raw.substr(offset+4);
        assert(sscanf(headers.c_str(),"HTTP/1.1 %d",&code)==1);
        assert(headers.find("Cache-Control: no-store")!=string::npos);
        if(headers.find("Transfer-Encoding: chunked")==string::npos){
            auto it=headers.find("Content-Length: ");assert(it!=string::npos&&std::stoul(headers.substr(it+16))==body.size());
        }
    }
    string cookie()const{
        const string prefix="Set-Cookie: zt_session=";auto i=headers.find(prefix);assert(i!=string::npos);
        auto token=headers.substr(i+prefix.size(),32);assert(headers.find("; Path=/api/v1; HttpOnly; SameSite=Strict")!=string::npos);
        return token;
    }
};
struct Server{
    web::HttpMessaging http;Backend b;int listener; sockaddr_in address{};size_t fragment=128;
    Server(){
        address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
        assert(!bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))&&!listen(listener,4));
        socklen_t n=sizeof(address);assert(!getsockname(listener,reinterpret_cast<sockaddr*>(&address),&n));
    }
    ~Server(){close(listener);}
    int arrive(const string &request){
        int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
        assert(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)));Socket client{fd,0};
        assert(client.write(request.data(),request.size())&&!shutdown(fd,SHUT_WR));return fd;
    }
    Response finish(int client){
        sockaddr_in peer{};socklen_t n=sizeof(peer);int fd=accept(listener,reinterpret_cast<sockaddr*>(&peer),&n);assert(fd>=0);
        assert(peer.sin_addr.s_addr==htonl(INADDR_LOOPBACK));Socket io{fd,fragment};
        http.handle(io,b,peer.sin_addr.s_addr,"127.0.0.1",{web::MessagingPage,sizeof(web::MessagingPage)-1,web::MessagingCsp});
        shutdown(fd,SHUT_WR);close(fd);char packet[512];string result;
        for(;;){auto count=recv(client,packet,sizeof(packet),0);if(count<=0)break;result.append(packet,size_t(count));}close(client);
        return Response(result);
    }
    Response call(const string &request){return finish(arrive(request));}
};
const string get="GET /api/v1/fromradio HTTP/1.1\r\nHost: 127.0.0.1\r\n",put="PUT /api/v1/toradio HTTP/1.1\r\nHost: 127.0.0.1\r\n";
static string request(const string &body,const string &cookie={},const string &extra={}){
    return put+"Content-Type: application/x-protobuf\r\nContent-Length: "+std::to_string(body.size())+"\r\n"+
        (cookie.empty()?string{}:"Cookie: zt_session="+cookie+"\r\n")+extra+"\r\n"+body;
}
static string encode(const meshtastic_ToRadio &r){
    uint8_t out[512];size_t n;assert(web::encode(meshtastic_ToRadio_fields,&r,out,sizeof(out),n));return {(char*)out,n};
}
static meshtastic_FromRadio decode(const string &s){
    meshtastic_FromRadio r{};auto in=pb_istream_from_buffer(reinterpret_cast<const uint8_t*>(s.data()),s.size());
    assert(pb_decode(&in,meshtastic_FromRadio_fields,&r));return r;
}
static meshtastic_ToRadio textPacket(uint32_t id,meshtastic_PortNum port=meshtastic_PortNum_TEXT_MESSAGE_APP){
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
    p.to=UINT32_MAX;p.id=id;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=port;p.decoded.payload.size=2;memcpy(p.decoded.payload.bytes,"hi",2);return r;
}
static string cookieHeader(const string &token){return "Cookie: zt_session="+token+"\r\n\r\n";}
int main(){
    (void)&results;Server s;auto &b=s.b;
    mt::ChannelRecord secondChannel;uint8_t channelKey[16]={77,13};
    assert(secondChannel.set(2,"Browser",channelKey,sizeof(channelKey),0,false,false,false));
    assert(b.fixture.s.config.setSecondaryChannel(3,secondChannel));
    assert(s.call("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").body==web::MessagingPage);
    auto page=s.call("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(page.headers.find(web::MessagingCsp)!=string::npos&&page.headers.find("text/html; charset=utf-8")!=string::npos);
    assert(s.call(get+"\r\n").body.empty() && !b.opens);
    meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=715;
    const auto start=encode(hello);
    assert(s.call(request(encode(textPacket(1)))).code==403&&!b.accepted);
    assert(s.call(request(start,"","Origin: http://other.local\r\n")).code==403&&!b.opens);
    assert(s.call(request(start,"","Sec-Fetch-Site: cross-site\r\n")).code==403&&!b.opens);
    assert(s.call("GET / HTTP/1.1\r\nHost: other.local\r\n\r\n").code==403);
    b.local=false;assert(s.call(request(start)).code==403&&!b.opens);b.local=true;
    b.entropy=false;assert(s.call(request(start)).code==503&&!b.opens);b.entropy=true;
    b.raw=true;assert(s.call(request(start)).code==409&&!b.opens);b.raw=false;
    b.capacity=false;assert(s.call(request(start)).code==503&&b.opens==1&&!s.http.session.generation);b.capacity=true;
    s.fragment=1;
    int first=s.arrive(request(start,"","Origin: http://127.0.0.1\r\n")),second=s.arrive(request(start));
    auto opened=s.finish(first);assert(opened.code==200 && opened.body==start);
    const string cookie=opened.cookie();
    assert(s.finish(second).code==409 && s.call(get+"\r\n").code==409);
    assert(s.call(get+cookieHeader(string(32,'a'))).code==409);
    assert(s.http.session.matches(htonl(INADDR_LOOPBACK),cookie.c_str())&&!s.http.session.matches(0,cookie.c_str()));
    assert(b.accepted==1&&b.host.secrets==1&&b.host.saves==0);
    s.fragment=600;
    unsigned seen=0,primary=0,secondary=0,complete=0;
    for(;;){
        auto out=s.call(get+cookieHeader(cookie));assert(out.code==200&&out.body.find("fixture-protected-network-setting")==string::npos);
        if(out.body.empty())break;auto msg=decode(out.body);++seen;
        switch(msg.which_payload_variant){
        case meshtastic_FromRadio_channel_tag:
            assert(!msg.channel.settings.psk.size&&!msg.channel.settings.uplink_enabled&&!msg.channel.settings.downlink_enabled);
            secondary+=msg.channel.index==3&&msg.channel.role==meshtastic_Channel_Role_SECONDARY&&!strcmp(msg.channel.settings.name,"Browser");
            primary+=!msg.channel.index&&msg.channel.role==meshtastic_Channel_Role_PRIMARY;break;
        case meshtastic_FromRadio_config_complete_id_tag:assert(msg.config_complete_id==715);++complete;break;
        case meshtastic_FromRadio_my_info_tag:assert(msg.my_info.my_node_num==OURS);break;
        case meshtastic_FromRadio_node_info_tag:assert(!msg.node_info.has_position);break;
        case meshtastic_FromRadio_queueStatus_tag:break;
        default:assert(false);
        }
    }
    assert(seen>=5&&primary==1&&secondary==1&&complete==1&&!b.host.saves);
    // Future peer hardware model 300 exceeds the stock C++ enum range and its
    // storage under -fshort-enums. Both builds must retain the public labels.
    const uint8_t futureNode[]={0x94,0xc3,0,20,8,11,34,16,8,42,18,12,18,3,'N','e','w',26,2,'N','W',40,0xac,2};
    static_assert(sizeof(futureNode)==24);
    assert(b.host.frames.pushFramed(futureNode,sizeof(futureNode)));
    auto label=decode(s.call(get+cookieHeader(cookie)).body);
    assert(label.id==11&&label.which_payload_variant==meshtastic_FromRadio_node_info_tag&&label.node_info.num==42&&label.node_info.has_user);
    assert(!strcmp(label.node_info.user.long_name,"New")&&!strcmp(label.node_info.user.short_name,"NW")&&
           label.node_info.user.hw_model==meshtastic_HardwareModel_UNSET);
    group("onboard page, two concurrent loopback browsers, actual owner-2 handshake and credential-free discovery");
    meshtastic_ToRadio ping{};ping.which_payload_variant=meshtastic_ToRadio_heartbeat_tag;ping.heartbeat.nonce=1;
    const auto beforePing=b.accepted;
    assert(s.call(request(encode(ping),cookie)).code==403&&b.accepted==beforePing);
    const auto accepted=b.accepted,opens=b.opens;
    for(auto port:{meshtastic_PortNum_ADMIN_APP,meshtastic_PortNum_REMOTE_HARDWARE_APP,meshtastic_PortNum_SERIAL_APP,meshtastic_PortNum_WAYPOINT_APP}){
        assert(s.call(request(encode(textPacket(71,port)),cookie)).code==403);
    }
    auto forbidden=textPacket(73);
    memcpy(forbidden.packet.decoded.payload.bytes,"zt_",3);forbidden.packet.decoded.payload.size=3;
    assert(s.call(request(encode(forbidden),cookie)).code==403);
    auto invalid=textPacket(73);invalid.packet.from=PEER;assert(s.call(request(encode(invalid),cookie)).code==403);
    auto future=textPacket(74);
    memset(&future.packet.priority,0xff,sizeof(future.packet.priority));
    const auto unsupportedPriority=s.call(request(encode(future),cookie)).code;
    assert(unsupportedPriority==403||unsupportedPriority==400);
    future=textPacket(75);memset(&future.packet.decoded.portnum,0xff,sizeof(future.packet.decoded.portnum));
    const auto unsupportedPort=s.call(request(encode(future),cookie)).code;
    assert(unsupportedPort==403||unsupportedPort==400);
    meshtastic_ToRadio file{};file.which_payload_variant=meshtastic_ToRadio_xmodemPacket_tag;
    assert(s.call(request(encode(file),cookie)).code==403);
    assert(s.call(request(start+start,cookie)).code==400);
    assert(s.call(put+"Content-Length: 513\r\n\r\n").code==413);
    assert(s.call(put+"Content-Length: 3\r\nContent-Length: 3\r\n\r\n").code==400);
    assert(s.call(put+"Transfer-Encoding: chunked\r\n\r\n").code==400);
    assert(s.call(put+"Content-Length: 3\r\n\r\nabc").code==415);
    assert(s.call(get+"X-Fill: "+string(4100,'a')+"\r\n\r\n").code==431);
    assert(s.call(request(start,cookie).substr(0,request(start,cookie).size()-1)).code==408);
    assert(b.accepted==accepted&&b.opens==opens&&b.host.frames.size()==0&&!b.host.saves);
    auto selected=textPacket(0x4321);selected.packet.channel=3;auto text=encode(selected);auto sent=s.call(request(text,cookie));assert(sent.code==200&&sent.body==text);
    assert(b.fixture.s.endpointFree()==3&&b.accepted==accepted+1);
    b.fixture.s.poll(300,0);b.fixture.done();b.companion.poll(mt::Companion::WorkspaceProof{true});
    mt::PrimaryChannel::Selection selectedKey;
    assert(b.fixture.s.config.channelSelection(3,selectedKey)&&b.fixture.radio.sends==1&&b.fixture.radio.frames[0][13]==selectedKey.hash);
    mt::PrimaryChannel::clear(selectedKey);
    auto onAir=b.fixture.wire(PEER,OURS,789,0,0,0,true);b.fixture.rx(onAir,510);
    b.companion.receive(onAir.data(),onAir.size(),-90,3,510,mt::Companion::WorkspaceProof{true});
    bool received=false;for(;;){auto reply=s.call(get+cookieHeader(cookie));if(reply.body.empty())break;
        auto msg=decode(reply.body);if(msg.which_payload_variant==meshtastic_FromRadio_packet_tag&&msg.packet.id==789){
            assert(msg.packet.from==PEER&&msg.packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&
                   msg.packet.decoded.payload.size==5&&!memcmp(msg.packet.decoded.payload.bytes,"Hello",5));received=true;}}
    assert(received);
    meshtastic_FromRadio f{};f.which_payload_variant=meshtastic_FromRadio_packet_tag;
    auto &p=f.packet;p.from=PEER;p.to=UINT32_MAX;p.id=2;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded=textPacket(2).packet.decoded;
    auto inject=[&](){uint8_t frame[516]={0x94,0xc3};size_t n;
        assert(web::encode(meshtastic_FromRadio_fields,&f,frame+4,512,n));frame[2]=uint8_t(n>>8);frame[3]=uint8_t(n);assert(b.host.send(frame,n+4));};
    inject();p.id=3;inject();
    auto all=s.call("GET /api/v1/fromradio?all=true HTTP/1.1\r\nHost: 127.0.0.1\r\n"+cookieHeader(cookie));
    assert(all.headers.find("Transfer-Encoding: chunked")!=string::npos);
    size_t i=0;unsigned delivered=0;
    for(;;){size_t end=all.body.find("\r\n",i);assert(end!=string::npos);auto n=std::stoul(all.body.substr(i,end-i),nullptr,16);i=end+2;
        if(!n)break;auto frame=decode(all.body.substr(i,n));i+=n;assert(all.body.substr(i,2)=="\r\n");i+=2;
        if(frame.which_payload_variant==meshtastic_FromRadio_packet_tag&&frame.packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP)++delivered;
    }
    assert(delivered==2&&all.body.substr(i)=="\r\n");
    assert(b.fixture.s.armEndpoint(false));auto disabled=textPacket(0xcafe);
    assert(s.call(request(encode(disabled),cookie)).code==200);bool rejection=false;
    for(;;){auto res=s.call(get+cookieHeader(cookie));if(res.body.empty())break;auto msg=decode(res.body);
        if(msg.which_payload_variant==meshtastic_FromRadio_clientNotification_tag){
            assert(msg.clientNotification.reply_id==0xcafe&&!strcmp(msg.clientNotification.message,"Radio is unavailable or TX is disarmed"));rejection=true;}
    }
    assert(rejection&&b.fixture.radio.sends==1&&!b.host.saves);
    group("bounded bare stock polling and chunk-all, authorized text, local disarmed feedback, protected operations refused");
    inject();b.clock+=web::HttpMessaging::IdleMs+1;assert(s.call(get+cookieHeader(cookie)).code==403 && !b.host.frames.size());
    auto next=s.call(request(start));assert(next.code==200&&next.cookie()!=cookie);
    assert(s.call(get+cookieHeader(cookie)).code==409);
    auto bye=meshtastic_ToRadio{};bye.which_payload_variant=meshtastic_ToRadio_disconnect_tag;bye.disconnect=true;
    assert(s.call(request(encode(bye),next.cookie())).code==200&&!s.http.session.generation);
    assert(s.call(get+"\r\n").body.empty()&&!b.host.saves);group("idle rotation, explicit disconnect, queue isolation and zero settings writes");
}
