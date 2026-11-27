// SPDX-License-Identifier: GPL-3.0-only
#include "modules/BeaconController.h"
#include "known_channel_fixture.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
using Bytes=std::vector<uint8_t>;
struct Radio:mt::Radio{
 int state=0;bool busy_=false,done=false;std::vector<Bytes> frames;
 int configure(const mt::Config &c)override{return c.valid()?0:-EINVAL;} // explicit simulated support for the changed-channel regression
 bool ready()override{return true;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}int stop()override{state=0;return 0;}int health()override{return state;}bool busy()override{return busy_;}
 uint32_t airtime(size_t)override{return 100;}int send(uint8_t*p,size_t n)override{frames.emplace_back(p,p+n);done=false;return 0;}bool completion(int &r)override{r=0;return done;}
};
struct Memory:mt::SettingsBackend{
 Radio &radio;bool fail=false;unsigned writes=0;std::map<std::string,Bytes> files;
 explicit Memory(Radio&r):radio(r){}
 int read(const char*k,uint8_t*p,size_t n)override{auto i=files.find(k);if(i==files.end())return -ENOENT;if(i->second.size()>n)return -E2BIG;memcpy(p,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char*k,const uint8_t*p,size_t n)override{assert(radio.state==0&&radio.frames.empty());++writes;if(fail)return -EIO;files[k]={p,p+n};return 0;}
};
struct Fixture{
 Radio radio;Memory memory;mt::test::KnownChannelFixtureClock clock;mt::Service service;mt::modules::ExtendedModuleJournal journal;mt::modules::ExtendedModuleEdit edit;mt::modules::BeaconController controller;
 std::vector<Bytes> local;std::vector<uint32_t> ids;
 Fixture():memory(radio),service(radio,true,0x12345678,127,true,false),edit(service,journal,memory){
  clock.bind(service);mt::Config cfg;cfg.bytes[4]=1;cfg.setTxEnabled(true);auto record=cfg.node();record.setPublic();cfg.node(record);assert(service.apply(cfg)&&service.start(false,0)&&service.armEndpoint(true));assert(!journal.load(memory)&&edit.load());
 }
 void config(bool offer=false,bool split=true){meshtastic_ModuleConfig c{};c.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;auto &b=c.payload_variant.mesh_beacon;b.flags=3|(split?4:0);strcpy(b.broadcast_message,"hello");if(offer){b.has_broadcast_offer_channel=true;strcpy(b.broadcast_offer_channel.name,"Invite");}assert(!edit.save(c,10,false));}
 void poll(uint64_t now,bool ready=true){assert(clock.set(now));controller.poll(service,edit,ready,now,0,1000+uint32_t(now),[&](const uint8_t*p,size_t n,uint32_t id){local.emplace_back(p,p+n);ids.push_back(id);});}
 void send(uint32_t now){service.poll(now,1);}
};
static void frequencySlots(){
 Fixture f;
 auto &core=f.service.config;
 auto settings=core.radioRequest();settings.regionCode=1;settings.channel=20;
 assert(core.setRadio(settings));
 mt::ChannelRecord secondary;uint8_t publicAlias=1;
 assert(secondary.set(2,"Secondary",&publicAlias,1,0,false,false,false,false));
 assert(core.setSecondaryChannel(1,secondary));
 meshtastic_ModuleConfig_MeshBeaconConfig cfg{};
 cfg.broadcast_targets_count=4;
 cfg.broadcast_targets[0].has_channel_index=true;cfg.broadcast_targets[0].channel_index=7; // disabled
 cfg.broadcast_targets[1].has_channel_index=true;cfg.broadcast_targets[1].channel_index=1;
 cfg.broadcast_targets[2]=cfg.broadcast_targets[1];
 cfg.broadcast_targets[3]=cfg.broadcast_targets[1];cfg.broadcast_targets[3].has_frequency_slot=true;
 cfg.broadcast_targets[3].frequency_slot=21;
 mt::modules::BeaconExcursion::ResolvedTarget targets[4]{};
 size_t count=0;
 assert(mt::modules::BeaconController::resolveTargets(core,cfg,targets,count)&&count==2);
 assert(targets[0].profile.channelIndex==1&&targets[0].profile.slot==20&&!targets[0].requiresRetune);
 assert(targets[1].profile.channelIndex==1&&targets[1].profile.slot==21&&targets[1].requiresRetune);
 cfg.broadcast_targets[3].frequency_slot=UINT32_MAX;
 assert(mt::modules::BeaconController::resolveTargets(core,cfg,targets,count)&&count==1);
 cfg.has_broadcast_offer_frequency_slot=true;cfg.broadcast_offer_frequency_slot=1;
 mt::beacon::BeaconPayloadBatch batch{};
 assert(mt::stock::MeshBeaconBroadcastModule::sendBeacon(cfg,batch,1)&&batch.count==1);
 auto &packet=batch.packets[0];meshtastic_MeshBeacon offer{};
 auto input=pb_istream_from_buffer(packet.payload.bytes,packet.payload.size);
 assert(pb_decode(&input,meshtastic_MeshBeacon_fields,&offer)&&offer.has_offer_frequency_slot&&offer.offer_frequency_slot==1);
 assert(mt::beacon::canonicalPayload(packet.payload.bytes,packet.payload.size));
 assert(mt::stock::MeshBeaconListenerModule::handleReceivedProtobuf(&offer,[](const auto &v){return v.has_offer_frequency_slot;}));
 cfg.broadcast_offer_frequency_slot=UINT32_MAX;cfg.has_broadcast_offer_channel=true;
 strcpy(cfg.broadcast_message,"text survives");
 assert(mt::stock::MeshBeaconBroadcastModule::sendBeacon(cfg,batch,1)&&batch.count==1&&batch.packets[0].portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
}
int main(){
 frequencySlots();
 {
  Fixture f;f.config();f.poll(44999);assert(!f.edit.config().sentBefore()&&f.local.empty());f.poll(60000);assert(f.edit.config().sentBefore()&&f.edit.config().lastSentUtc()==0&&f.local.size()==1&&f.memory.writes==2&&f.service.moduleAnnouncementPending()&&f.radio.frames.empty());
  f.send(61000);assert(f.radio.frames.size()==1);bench::RouteMetadata m;assert(f.service.codec.decode(f.radio.frames[0].data(),f.radio.frames[0].size(),m)&&m.portnum==1&&m.payloadLength==5&&!memcmp(m.payload,"hello",5)&&m.hasBitfield&&!m.bitfield&&!m.wantResponse&&!m.request_id&&!f.radio.frames[0][12]);
  f.radio.done=true;f.send(61001);f.poll(120000);assert(f.local.size()==1&&f.memory.writes==2);
  mt::modules::BeaconController rebooted;unsigned count=0;rebooted.poll(f.service,f.edit,true,120001,0,42,[&](const uint8_t*,size_t,uint32_t){++count;});assert(!count&&f.memory.writes==2);
 }
 {
  Fixture f;f.config();f.memory.fail=true;f.poll(60000);assert(f.local.empty()&&f.radio.frames.empty()&&!f.edit.ready()&&!f.service.receiving());
 }
 {
  Fixture f;f.config(true);f.poll(60000);assert(f.local.size()==1&&f.memory.writes==2);f.poll(60001);assert(f.local.size()==1);f.send(61000);assert(f.radio.frames.size()==1);f.radio.done=true;f.send(61001);f.poll(62000);assert(f.local.size()==2&&f.memory.writes==2&&f.ids[0]!=f.ids[1]);
  meshtastic_Data first{},second{};auto a=pb_istream_from_buffer(f.local[0].data(),f.local[0].size());auto b=pb_istream_from_buffer(f.local[1].data(),f.local[1].size());assert(pb_decode(&a,meshtastic_Data_fields,&first)&&pb_decode(&b,meshtastic_Data_fields,&second)&&first.portnum==37&&second.portnum==1);assert(mt::beacon::canonicalPayload(first.payload.bytes,first.payload.size));
 }
 {
  Fixture f;f.config(true);f.poll(60000);f.poll(66000);f.send(67000);assert(f.local.size()==1);f.radio.done=true;f.send(67001);f.poll(68000);assert(f.local.size()==1); // expired second half never resurfaces
 }
 {
  Fixture f;f.config(true);f.poll(60000);f.poll(60001,false);f.send(61000);f.radio.done=true;f.send(61001);f.poll(62000);assert(f.local.size()==1); // edit/reboot window cancels pending split
 }
 for(unsigned change=0;change<3;++change){
  Fixture f;f.config(true);f.poll(60000);f.send(61000);f.radio.done=true;f.send(61001);assert(f.radio.frames.size()==1);
  auto next=f.service.config;
  if(change==0)assert(next.setPrimaryName("Changed"));
  else if(change==1){auto request=next.radioRequest();request.channel=21;assert(next.setRadio(request));}
  else assert(next.setHopLimit(1));
  assert(!f.service.pauseForStorage()&&f.service.apply(next)&&f.service.start(false,61500)&&f.service.armEndpoint(true));
  f.poll(62000);f.send(63000);f.poll(64000);assert(f.local.size()==1&&f.radio.frames.size()==1&&f.memory.writes==2); // direct healthy save cancels pending half
 }
}
