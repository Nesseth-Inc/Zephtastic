// SPDX-License-Identifier: GPL-3.0-only
#include "modules/TrafficNodeInfoResponder.h"
#include <cassert>
#include <cstring>
#include <pb_encode.h>

void exerciseResponder() {
    using namespace mt::modules;
    using Result=TrafficNodeInfoResponder::Result;
    TrafficManagementSettings settings;
    TrafficNodeInfoResponder responder;
    assert(responder.configure(settings) && !responder.enabled());
    meshtastic_User user{};
    strcpy(user.id,"!000000ab"); strcpy(user.long_name,"Synthetic"); strcpy(user.short_name,"test");
    uint8_t wire[128]{};
    auto stream=pb_ostream_from_buffer(wire,sizeof(wire));
    assert(pb_encode(&stream,meshtastic_User_fields,&user));
    assert(!responder.observePublicUser(0xac,wire,stream.bytes_written,1,2,17,true));
    assert(!responder.observePublicUser(0xab,wire,stream.bytes_written,1,2,17,false));
    assert(responder.observePublicUser(0xab,wire,stream.bytes_written,1,2,17,true));
    TrafficNodeInfoRequest request{};
    request.requester=0xc0; request.target=0xab; request.requestId=4; request.nowMs=2;
    request.channelIndex=2; request.channelHash=17; request.hopsAway=0;
    request.requestShapeValid=request.replayAdmitted=request.publicRequest=true;
    struct Sink { bool ready=true,allocate=true,send=false; unsigned accepted=0; } sink;
    TrafficNodeInfoOwner owner{};
    owner.context=&sink;
    owner.proxyRouteAvailable=[](void *s){return static_cast<Sink *>(s)->ready;};
    owner.allocatePublicPacketId=[](void *s,uint32_t &id){id=40;return static_cast<Sink *>(s)->allocate;};
    owner.submitPublicProxy=[](void *s,const TrafficNodeInfoReply &reply){
        assert(reply.destination==0xc0 && reply.proxyAuthor==0xab && reply.unsignedPublicProxy &&
               reply.packetId==40 && reply.requestId==4 && reply.hopLimit==0 && reply.hopStart==0 &&
               reply.channelIndex==2 && reply.channelHash==17);
        auto &sink=*static_cast<Sink *>(s);
        if (sink.send) ++sink.accepted;
        return sink.send;
    };
    auto respond=[&](){return responder.respond(request,9,owner);};
    assert(respond()==Result::Disabled);
    meshtastic_ModuleConfig_TrafficManagementConfig c{};
    c.nodeinfo_direct_response_max_hops=2; settings.set(c);
    assert(responder.configure(settings) && responder.enabled());
    request.publicRequest=false; assert(respond()==Result::PolicyDenied); request.publicRequest=true;
    request.replayAdmitted=false; assert(respond()==Result::PolicyDenied); request.replayAdmitted=true;
    request.targetMuted=true; assert(respond()==Result::PolicyDenied); request.targetMuted=false;
    request.channelIndex=1; assert(respond()==Result::ChannelMismatch); request.channelIndex=2;
    request.hopsAway=1; assert(respond()==Result::HopLimit); request.hopsAway=0;
    sink.ready=false; assert(respond()==Result::OwnerUnavailable); sink.ready=true;
    sink.allocate=false; assert(respond()==Result::NoPacketId); sink.allocate=true;
    assert(respond()==Result::QueueFailed); sink.send=true;
    assert(respond()==Result::Replied && sink.accepted==1);
    assert(respond()==Result::RateLimited);
    request.nowMs+=60001; assert(respond()==Result::RateLimited); // replay window is longer.
    request.nowMs+=600000; request.routerRole=true; request.hopsAway=3; assert(respond()==Result::HopLimit);
    request.hopsAway=2; assert(respond()==Result::Replied && sink.accepted==2);
    request.nowMs+=TrafficNodeInfoResponder::MaxCacheAgeMs; assert(respond()==Result::NoFreshCache);
}
