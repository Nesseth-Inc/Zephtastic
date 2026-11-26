// SPDX-License-Identifier: GPL-3.0-only
// Regression from independently reproduced M112-SRC-002.
#include "SettingsCommit.h"
#include "known_channel_fixture.h"
#include <pb_encode.h>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using J=mt::modules::ExtendedModuleJournal;
constexpr uint32_t Self=0x12345678,Peer=0x24680135;
struct Backend:mt::SettingsBackend {
 std::map<std::string,std::vector<uint8_t>> files;unsigned writes=0;bool fail=false;
 int read(const char*k,uint8_t*p,size_t n)override{auto i=files.find(k);if(i==files.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char*k,const uint8_t*p,size_t n)override{++writes;if(fail)return -EIO;files[k]={p,p+n};return 0;}
};
struct Radio:mt::Radio{
 int state=0;unsigned sends=0;
 bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}int send(uint8_t*,size_t)override{++sends;return 0;}bool completion(int&)override{return false;}
};

#include "NodeAdmin.h"
int main(){
 Backend backend;Radio radio;mt::Service service(radio,true,Self,127,true,false);mt::test::KnownChannelFixtureClock clock;clock.bind(service);
 mt::Config config;config.bytes[4]=1;assert(config.setDeviceRole(1,0));auto node=config.node();node.setPublic();config.node(node);assert(service.apply(config));mt::NodeJournal journal;assert(!journal.load(backend,service.nodes,Self));assert(service.start(false,0));
 mt::NodeCache::Entry peer;peer.identity(Peer,127,0);assert(service.nodes.accept(peer,Self,0,0)>=0);
 mt::modules::TrafficManagementSettings settings;mt::modules::TrafficManagementWindow window;window.configure(settings);
 meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=418000000;pos.longitude_i=-876000000;
 meshtastic_Data d{};d.has_bitfield=true;d.portnum=meshtastic_PortNum_POSITION_APP;auto body=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&body,meshtastic_Position_fields,&pos));d.payload.size=body.bytes_written;
 uint8_t bytes[239],frame[255];auto data=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&data,meshtastic_Data_fields,&d));
 auto rx=[&](uint32_t id,uint64_t uptime){size_t n=0;assert(service.codec.encodeZeroHopData(Peer,UINT32_MAX,id,bytes,data.bytes_written,frame,sizeof(frame),n));service.trafficManagement(&window,uptime);service.rx(frame,n,0,uint32_t(uptime),123);return service.trafficSuppressed();};
 assert(!rx(1,0));
 assert(mt::NodeAdmin::edit(service,radio,journal,backend,Peer,mt::NodeCache::Edit::Remove,1,false,[](uint32_t id,void *ctx){static_cast<mt::modules::TrafficManagementWindow*>(ctx)->forget(id);},&window)==0);
 assert(service.nodes.find(Peer)<0&&service.receiving());assert(service.nodes.accept(peer,Self,0,0)>=0);
 const bool suppressed=rx(2,360000);printf("verified NodeAdmin::Remove and re-admitted same source; first new Position suppressed=%d (retired traffic slot was cleared)\n",int(suppressed));assert(!suppressed);
}
