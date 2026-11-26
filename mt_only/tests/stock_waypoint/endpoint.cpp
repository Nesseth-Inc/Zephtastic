// SPDX-License-Identifier: GPL-3.0-only
#include "position_query_fixture.h"
#include "stock/waypoint/WaypointModule.h"

void endpointCases(){
    using namespace mt::stock::waypoint;
    auto f=std::make_unique<Fixture>();
    WaypointModule state;
    state.domain(OURS,1);
    struct Capture:Host{
        WaypointModule &state;
        unsigned accepted=0,entered=0,positions=0;
        Capture(mt::Service &s,WaypointModule &w):Host(s),state(w){}
        void waypointPacket(uint32_t from,uint32_t to,uint32_t id,unsigned channel,bool priv,bool local,
                            const uint8_t *p,size_t n)override{
            if(state.handleReceived(from,id,{priv?(local?to:from):0,uint8_t(channel)},local,p,n,200)==Result::Stored)++accepted;
        }
        void positionForGeofence(uint32_t from,uint32_t,unsigned ch,bool priv,const uint8_t *p,size_t n)override{
            ++positions;state.evaluatePosition(from,{priv?from:0,uint8_t(ch)},p,n,200,false,[&](const Entry&,Crossing c){if(c==Crossing::Enter)++entered;});
        }
    } usb(f->s,state),ble(f->s,state);
    mt::Companion a(f->s,usb,81,f->workspace,0),b(f->s,ble,81,f->workspace,1);
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;
    input(a,r,1);input(b,r,1);
    r={};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &mp=r.packet;mp.id=71;mp.to=UINT32_MAX;mp.hop_limit=1;mp.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    auto &data=mp.decoded;data.portnum=meshtastic_PortNum_WAYPOINT_APP;
    meshtastic_Waypoint wp{};wp.id=123;strcpy(wp.name,"Zone");wp.notify_on_enter=true;wp.has_bounding_box=true;
    wp.bounding_box.latitude_north_i=wp.bounding_box.longitude_east_i=105;
    wp.bounding_box.latitude_south_i=wp.bounding_box.longitude_west_i=100;
    auto enc=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));assert(pb_encode(&enc,meshtastic_Waypoint_fields,&wp));
    data.payload.size=enc.bytes_written;
    input(b,r,10);assert(ble.accepted==1&&state.count()==1&&state.newest()->notificationPreferences==1);
    f->s.poll(300,0);assert(!f->radio.frames.empty());
    bench::RouteMetadata onAir{};const auto &tx=f->radio.frames.back();
    assert(f->s.codec.decode(tx.data(),tx.size(),onAir));
    meshtastic_Waypoint received{};auto air=pb_istream_from_buffer(onAir.payload,onAir.payloadLength);
    assert(pb_decode(&air,meshtastic_Waypoint_fields,&received));
    assert(received.id==wp.id&&received.has_bounding_box&&!received.notify_on_enter&&!received.notify_on_exit&&!received.notify_favorites_only);
    auto receive=[&](meshtastic_Data &d,uint32_t id,uint32_t to){
        uint8_t bytes[239],frame[255];d.has_bitfield=true;
        auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Data_fields,&d));
        size_t count=0;assert(f->s.codec.encodeZeroHopData(PEER,to,id,bytes,out.bytes_written,frame,sizeof(frame),count));
        f->s.rx(frame,count,3,400+id,600+id);
        for(auto *c:{&a,&b})c->receive(frame,count,-90,3,400+id,mt::Companion::WorkspaceProof{true});
    };
    receive(data,81,UINT32_MAX);
    assert(usb.accepted+ble.accepted==2&&state.count()==1&&state.newest()->notificationPreferences==1);
    auto position=[&](int32_t xy,uint32_t id,uint32_t to){
        meshtastic_Data d{};d.portnum=meshtastic_PortNum_POSITION_APP;meshtastic_Position p{};
        p.has_latitude_i=p.has_longitude_i=true;p.latitude_i=p.longitude_i=xy;
        auto o=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&o,meshtastic_Position_fields,&p));
        d.payload.size=o.bytes_written;receive(d,id,to);
    };
    position(90,83,UINT32_MAX);position(101,84,OURS+1);assert(!usb.entered&&!ble.entered);
    position(101,85,UINT32_MAX);assert(usb.entered+ble.entered==1);
    data.payload.bytes[0]=0xff;data.payload.size=1;receive(data,86,UINT32_MAX);
    assert(usb.accepted+ble.accepted==2);
    auto q=std::make_unique<Fixture>();
    WaypointModule replyState;replyState.domain(OURS,1);
    Capture observer(q->s,replyState);
    mt::Companion reply(q->s,observer,81,q->workspace,0);
    meshtastic_ToRadio path{};path.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;path.want_config_id=2;
    input(reply,path,1);input(reply,query(),100);q->s.poll(300,0);assert(q->radio.sends==1);q->done();
    for(uint32_t request:{999u,77u}){
      auto frame=goodFrame(request,900+request);
      q->s.rx(frame.data(),frame.size(),3,1500+request,55);
      reply.receive(frame.data(),frame.size(),-90,3,1500+request,mt::Companion::WorkspaceProof{true});
      assert(observer.positions==unsigned(request==77));
    }
    auto &service=q->s;service.stop();
    const auto epoch=service.waypointDomainEpoch();auto config=service.config;
    config.setOwnerUnmessagable(true);config.setDisplayLongNames(true);
    auto node=config.node();strcpy(reinterpret_cast<char*>(node.bytes+bench::MtNodeRecord::Name),"Updated node");
    node.seal();config.node(node);
    assert(service.apply(config)&&service.waypointDomainEpoch()==epoch);
    assert(config.setPrimaryName("Survey")&&service.apply(config)&&service.waypointDomainEpoch()!=epoch);
    replyState.domain(OURS,service.waypointDomainEpoch());assert(!replyState.count());
    puts("PASS loaded real Companion, USB/BLE single history, authored waypoint, admitted RF position, third-party and malformed frames");
}
