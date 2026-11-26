// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include <algorithm>

void satelliteCases(){
    auto f=std::make_unique<Fixture>();
    mt::NodeCache::Entry node;
    node.identity(PEER,81,0);
    assert(f->s.nodes.accept(node,OURS,3,1)>=0);
    uint32_t id=100;
    auto receive=[&](meshtastic_PortNum port,const pb_msgdesc_t *fields,const void *value,uint32_t to=UINT32_MAX){
        meshtastic_Data data{};data.portnum=port;data.has_bitfield=true;
        auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
        assert(pb_encode(&payload,fields,value));data.payload.size=payload.bytes_written;
        uint8_t bytes[239],frame[255];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));
        assert(pb_encode(&out,meshtastic_Data_fields,&data));size_t size=0;
        assert(f->s.codec.encodeZeroHopData(PEER,to,id,bytes,out.bytes_written,frame,sizeof(frame),size));
        f->s.rx(frame,size,3,id,1000+id);
        f->c.receive(frame,size,-83,3,id,1700000000u,mt::Companion::WorkspaceProof{true});
        ++id;
    };
    meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=101;pos.longitude_i=102;
    pos.time=1699000000u;pos.has_ground_speed=true;pos.ground_speed=180;pos.sats_in_view=8;
    receive(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos);
    meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_device_metrics_tag;
    t.variant.device_metrics.has_battery_level=true;t.variant.device_metrics.battery_level=71;
    receive(meshtastic_PortNum_TELEMETRY_APP,meshtastic_Telemetry_fields,&t);
    t={};t.which_variant=meshtastic_Telemetry_environment_metrics_tag;
    t.variant.environment_metrics.has_temperature=true;t.variant.environment_metrics.temperature=20.25f;
    receive(meshtastic_PortNum_TELEMETRY_APP,meshtastic_Telemetry_fields,&t);
    meshtastic_StatusMessage status{};strcpy(status.status,"camp");
    receive(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&status);
    unsigned expectedPrecision=32;
    auto sync=[&](uint32_t nonce,unsigned nodes,bool config,unsigned samples){
        f->h.out.clear();meshtastic_ToRadio r{};
        r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=nonce;
        input(f->c,r,200);
        for(unsigned i=0;i<10;++i)f->c.poll(mt::Companion::WorkspaceProof{true});
        unsigned ns=0,cs=0,packets=0;bool complete=false;
        for(const auto &msg:f->h.out){
            switch(msg.which_payload_variant){
            case meshtastic_FromRadio_node_info_tag:
                assert(!complete);if(msg.node_info.num==PEER)assert(!msg.node_info.has_position&&!msg.node_info.has_device_metrics);
                ++ns;break;
            case meshtastic_FromRadio_config_tag:assert(!complete);++cs;break;
            case meshtastic_FromRadio_config_complete_id_tag:assert(!complete&&msg.config_complete_id==nonce);complete=true;break;
            case meshtastic_FromRadio_packet_tag:{
                const auto &p=msg.packet;assert(complete&&p.from==PEER&&p.to==UINT32_MAX&&p.has_rx_time&&p.rx_time==1700000000u&&!p.has_rx_rssi);
                const meshtastic_PortNum ports[]={meshtastic_PortNum_POSITION_APP,meshtastic_PortNum_TELEMETRY_APP,
                    meshtastic_PortNum_TELEMETRY_APP,meshtastic_PortNum_NODE_STATUS_APP};
                assert(packets<4&&p.decoded.portnum==ports[packets]);
                if(!packets){
                    auto input=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);meshtastic_Position got{};
                    assert(pb_decode(&input,meshtastic_Position_fields,&got)&&got.precision_bits==expectedPrecision);
                    assert(got.has_latitude_i&&got.latitude_i==101&&got.has_longitude_i&&got.longitude_i==102);
                    assert(!got.has_altitude&&!got.has_ground_speed&&!got.sats_in_view&&got.time==1699000000u);
                }
                if(packets==1||packets==2){
                    auto input=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);
                    meshtastic_Telemetry got{};assert(pb_decode(&input,meshtastic_Telemetry_fields,&got)&&got.time==p.rx_time);
                    if(packets==1)assert(got.which_variant==meshtastic_Telemetry_device_metrics_tag&&got.variant.device_metrics.battery_level==71);
                    else assert(got.which_variant==meshtastic_Telemetry_environment_metrics_tag&&got.variant.environment_metrics.temperature==20.25f);
                }
                ++packets;break;}
            default:assert(nonce!=69421);break;
            }
        }
        assert(complete&&ns==nodes&&(cs!=0)==config&&packets==samples);
    };
    sync(69420,1,true,0);sync(69421,2,false,4);sync(99,2,true,4);
    pos.time=0;pos.precision_bits=16;expectedPrecision=16;
    receive(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos);sync(69421,2,false,4);
    strcpy(status.status,"direct");
    receive(meshtastic_PortNum_NODE_STATUS_APP,meshtastic_StatusMessage_fields,&status,OURS);
    sync(69421,2,false,3);
    node.bytes[node.KeyLength]=32;memset(node.bytes+node.Key,17,32);
    assert(f->s.nodes.ownerContact(node,OURS,false,true)>=0);sync(69421,2,false,0);
    receive(meshtastic_PortNum_POSITION_APP,meshtastic_Position_fields,&pos);
    assert(f->s.nodes.edit(PEER,OURS,mt::NodeCache::Edit::Ignore)==1);
    assert(f->s.nodes.edit(PEER,OURS,mt::NodeCache::Edit::Unignore)==1);sync(69421,2,false,0);
    assert(f->s.nodes.edit(PEER,OURS,mt::NodeCache::Edit::Remove)==1);
    assert(f->s.nodes.accept(node,OURS,3,1)>=0);sync(69421,2,false,0);
    puts("PASS selective PhoneAPI sync, public satellite replay order, payloads, timestamps, and re-enrolled peer isolation");
}
