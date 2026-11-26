// SPDX-License-Identifier: GPL-3.0-only
// Regression from independently reproduced M112-SRC-001.
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
int main(){
 Radio radio;mt::Service service(radio,true,Self,127,true,false);mt::test::KnownChannelFixtureClock clock;clock.bind(service);
 mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);assert(service.apply(config)&&service.start(false,0)&&service.armEndpoint(true));
 mt::NodeCache::Entry tracker;tracker.identity(Peer,127,5);assert(service.nodes.accept(tracker,Self,0,0)>=0);
 mt::modules::TrafficManagementSettings settings;meshtastic_ModuleConfig_TrafficManagementConfig cfg{};cfg.position_min_interval_secs=18000;settings.set(cfg);mt::modules::TrafficManagementWindow window;window.configure(settings);
 meshtastic_Position pos{};pos.has_latitude_i=pos.has_longitude_i=true;pos.latitude_i=418000000;pos.longitude_i=-876000000;
 meshtastic_Data d{};d.has_bitfield=true;d.portnum=meshtastic_PortNum_POSITION_APP;auto body=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&body,meshtastic_Position_fields,&pos));d.payload.size=body.bytes_written;
 uint8_t bytes[239],frame[255];auto data=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&data,meshtastic_Data_fields,&d));
 auto rx=[&](uint32_t id,uint64_t uptime){size_t n=0;assert(service.codec.encodeZeroHopData(Peer,UINT32_MAX,id,bytes,data.bytes_written,frame,sizeof(frame),n));service.trafficManagement(&window,uptime);service.rx(frame,n,0,uint32_t(uptime),123);return service.trafficSuppressed();};
 assert(!rx(1,0));assert(rx(2,9ull*360000));
 for(unsigned i=0;i<16;++i){mt::NodeCache::Entry e;e.identity(Peer+1+i,127,0);assert(service.nodes.accept(e,Self,0,0)>=0);}
 assert(service.nodes.find(Peer)<0);
 const bool suppressed=rx(3,10ull*360000);
 printf("tracker previously admitted with role=5; peer evicted; next duplicate at 60 minutes suppressed=%d (role cap should allow)\n",int(suppressed));
 assert(!suppressed); // Retained tracker role permits the one-hour refresh.
 assert(!rx(4,50ull*360000));
 // An admitted role change refreshes the tracked slot even if NodeDB evicts
 // the identity before the next position. CLIENT removes the tracker cap.
 window.observeRole(Peer,0);assert(rx(5,60ull*360000));assert(!rx(6,100ull*360000)); // Later refresh remains eligible.
}
