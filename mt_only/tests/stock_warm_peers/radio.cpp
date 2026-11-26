// SPDX-License-Identifier: GPL-3.0-only
#include "endpoint_fixture.h"
#include "NodeJournal.h"
#include "companion/WarmPeerOwner.h"
#include <map>
#include <string>
#include "Map.h"
using namespace mt::companion;
using E=mt::NodeCache::Entry;
static WarmPeerOwner<100> owner;
static unsigned calls=0;
struct Settings:mt::SettingsBackend{
    std::map<std::string,std::vector<uint8_t>> files;
    int read(const char *p,uint8_t *out,size_t n)override{
        auto i=files.find(p);if(i==files.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;
        memcpy(out,i->second.data(),i->second.size());return int(i->second.size());
    }
    int write(const char *p,const uint8_t *in,size_t n)override{files[p]={in,in+n};return 0;}
};
int main(){
    auto f=std::make_unique<Fixture>();WarmPeerDomain d{};d.self=OURS;d.owner[0]=2;d.epoch[0]=3;d.directory[0]=5;
    WarmPeerWithdrawals fence{};Map fs;owner.bind(d,fence);assert(!owner.poll(fs));
    auto e=[](uint32_t id,int key,uint32_t role){E e;e.identity(id,81,role);e.bytes[E::KeyLength]=32;memset(e.bytes+E::Key,key,32);return e;};
    auto put=[&](E previous){previous.bytes[E::LocalFlags]|=E::VerifiedSigner;
        owner.demote(previous.bytes,1700000127);for(int i=0;i<10;i++)assert(!owner.poll(fs));assert(owner.absorbed());owner.acknowledged();};
    put(e(PEER,3,5));
    f->s.warmPositionRole([](uint32_t id,uint8_t &role){++calls;return owner.positionRole(id,role);});
    mt::modules::TrafficManagementSettings settings;mt::modules::TrafficManagementWindow window;window.configure(settings);
    meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=418000000;pos.longitude_i=-876000000;
    meshtastic_Data data{};data.has_bitfield=true;data.portnum=meshtastic_PortNum_POSITION_APP;
    auto body=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));assert(pb_encode(&body,meshtastic_Position_fields,&pos));data.payload.size=body.bytes_written;
    uint8_t bytes[239];auto payload=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&payload,meshtastic_Data_fields,&data));
    uint32_t seq=0;
    auto rx=[&](uint32_t dest,uint64_t tick){
        size_t n=0;uint8_t frame[255];assert(f->s.codec.encodeZeroHopData(PEER,dest,++seq,bytes,payload.bytes_written,frame,sizeof(frame),n));
        const unsigned before=fs.reads;f->s.trafficManagement(&window,tick*360000);f->s.rx(frame,n,0,uint32_t(tick*360000),123);
        assert(before==fs.reads);return f->s.trafficSuppressed();
    };
    assert(!rx(OURS,0)&&calls==0);assert(!rx(UINT32_MAX,1)&&calls==1);assert(rx(UINT32_MAX,2));
    for(int i=0;i<8;i++)assert(!owner.poll(fs));
    assert(rx(UINT32_MAX,10));assert(!rx(UINT32_MAX,11)); // TRACKER refreshes at 60 min, CLIENT waits 5 h.
    assert(!f->s.previouslyVerifiedSigner(PEER)&&f->s.nodes.find(PEER)<0);
    auto hot=e(PEER,3,0);assert(f->s.nodes.accept(hot,OURS,-5,2)>=0);const unsigned called=calls;
    assert(rx(UINT32_MAX,21)&&calls==called); // fresh CLIENT overrides prior TRACKER
    mt::NodeJournal j;Settings mem;assert(!j.load(mem,f->s.nodes,OURS));j.enableReplayEpochs(f->s.nodes);
    assert(f->s.nodes.accept(hot,OURS,-5,1)>=0);assert(f->s.nodes.accept(e(PEER+1,8,6),OURS,-5,1)>=0);
    put(e(PEER+1,1,10)); // old owner had a different key
    const auto before=fs.reads;owner.capture(f->s.nodes);assert(before==fs.reads);
    assert(!f->s.nodes.replayEpoch(0));assert(j.flush(mem,f->s.nodes,55,true,true)==1);
    for(int i=0;i<22;i++){const auto before=fs.reads;owner.capture(f->s.nodes);assert(fs.reads==before);assert(!owner.poll(fs));}
    const auto &restored=*f->s.nodes.get(f->s.nodes.find(PEER));const auto &changed=*f->s.nodes.get(f->s.nodes.find(PEER+1));
    assert(restored.lastHeard()==(1700000127u&~127u)&&!restored.verifiedSigner()&&restored.role()==0&&restored.bytes[E::Key]==3);
    assert(!changed.lastHeard()&&!changed.verifiedSigner()&&changed.role()==6&&changed.bytes[E::Key]==8);
    puts("PASS real Service::rx timing, public broadcast only, fresh role priority, durable hot admission, re-key rejection and unchanged signer state");
}
