// SPDX-License-Identifier: GPL-3.0-only
// Run with scripts/test-pki-runtime.py for real PSA and the private/ham channel dispatch.
#include "pki_runtime_fixture.h"
#include <cstdio>
struct WaypointHost:Host {
    unsigned calls=0,channel=9;bool priv=false;
    void waypointPacket(uint32_t,uint32_t,uint32_t,unsigned ch,bool encrypted,bool local,
                        const uint8_t *bytes,size_t length) override{
        meshtastic_Waypoint w{};auto in=pb_istream_from_buffer(bytes,length);
        assert(local&&pb_decode(&in,meshtastic_Waypoint_fields,&w)&&w.notify_on_enter);
        ++calls;channel=ch;priv=encrypted;
    }
};
int main(){
    for(bool licensed:{false,true}){
        Fixture f(true);f.s.stop();
        auto cfg=f.s.config;mt::ChannelRecord c;
        assert(c.set(2,"Survey",nullptr,0,0,false,false,false,false));
        assert(cfg.setSecondaryChannel(1,c));
        if(licensed)assert(cfg.enableLicensedOperation());
        assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true));
        WaypointHost local;mt::Companion client(f.s,local,81,f.workspace,0);
        meshtastic_ToRadio init{};init.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;init.want_config_id=2;input(client,init,1);
        meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
        auto &p=r.packet;p.to=f.peer.binding.node;p.id=201;p.channel=1;p.hop_limit=1;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
        p.pki_encrypted=!licensed;p.decoded.portnum=meshtastic_PortNum_WAYPOINT_APP;
        meshtastic_Waypoint w{};w.id=88;w.notify_on_enter=w.notify_on_exit=w.notify_favorites_only=true;
        auto o=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
        assert(pb_encode(&o,meshtastic_Waypoint_fields,&w));p.decoded.payload.size=o.bytes_written;
        input(client,r,100);f.poll(1000);assert(local.calls==1 && local.priv==!licensed && local.channel==(licensed?1:0));
        assert(f.radio.frames.size()==1);const auto &wire=f.radio.frames.front();
        uint8_t plain[256];size_t size=0;
        if(!licensed)assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,201,wire.data()+16,wire.size()-16,plain,sizeof(plain),size));
        else {size=wire.size()-16;memcpy(plain,wire.data()+16,size);}
        meshtastic_Data d{};auto in=pb_istream_from_buffer(plain,size);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&d.portnum==meshtastic_PortNum_WAYPOINT_APP);
        assert((d.xeddsa_signature.size==64)==licensed);
        meshtastic_Waypoint actual{};in=pb_istream_from_buffer(d.payload.bytes,d.payload.size);
        assert(pb_decode(&in,meshtastic_Waypoint_fields,&actual)&&actual.id==88&&!actual.notify_on_enter&&!actual.notify_on_exit&&!actual.notify_favorites_only);
    }
    puts("PASS real PSA PKI on slot zero and signed licensed unicast on the selected secondary, all local waypoint flags removed over RF");
}
