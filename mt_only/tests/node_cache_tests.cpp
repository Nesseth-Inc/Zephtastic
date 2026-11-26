// SPDX-License-Identifier: GPL-3.0-only
#include "Companion.h"
#include "known_channel_fixture.h"
#include "NodeJournal.h"
#include "NodeAdmin.h"
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using Bytes=std::vector<uint8_t>;
using Cache=mt::NodeCache;using Journal=mt::NodeJournal;
static constexpr uint32_t Self=0x12345678,Peer=0x87654321;
struct Memory final:mt::SettingsBackend {
 std::map<std::string,Bytes> files;int writes=0,reads=0,cut=-1,readError=0,writeError=0;bool committedError=false,corruptReadback=false;
 int read(const char *key,uint8_t *out,size_t n)override{
 ++reads;if(readError)return readError;auto it=files.find(key);if(it==files.end())return -ENOENT;
  if(it->second.size()>n)return -E2BIG;
  if(!it->second.empty())memcpy(out,it->second.data(),it->second.size());
  return int(it->second.size());
 }
 int write(const char *key,const uint8_t *data,size_t n)override{
  ++writes;if(writeError)return writeError;
  if(cut>=0&&size_t(cut)<n){files[key]=Bytes(data,data+cut);return -EIO;}
  files[key]=Bytes(data,data+n);if(committedError)return -EIO;if(corruptReadback)files[key][100]^=1;return 0;
 }
};
static Cache::Entry entry(uint32_t id=Peer,const char *name="Synthetic peer",unsigned keySize=32,uint8_t keyByte=0x42){
 Cache::Entry e;e.identity(id,127,1);assert(Cache::Entry::text(e.bytes+Cache::Entry::Name,40,name,strlen(name)+1));
 assert(Cache::Entry::text(e.bytes+Cache::Entry::Short,5,"SYN",4));e.bytes[Cache::Entry::KeyLength]=uint8_t(keySize);
 memset(e.bytes+Cache::Entry::Key,keyByte,keySize);e.bytes[Cache::Entry::Flags]=1;return e;
}
static void seal(Bytes &b){uint32_t c=UINT32_MAX;for(size_t i=0;i<b.size()-4;++i){c^=b[i];for(unsigned bit=0;bit<8;++bit)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}Cache::Entry::put32(b.data()+b.size()-4,~c);}
static void generation(Bytes &b,uint32_t g){Cache::Entry::put32(b.data()+4,g);seal(b);}
static unsigned groups=0;
#define PASS(s) do{++groups;puts(s);}while(0)
struct Radio final:mt::Radio {
 unsigned sends=0;int state=0,stopError=0;bool available=true;
 bool ready()override{return available;}int start()override{state=1;return 0;}int restore()override{state=1;return 0;}
 int stop()override{if(!stopError)state=0;return stopError;}
 int health()override{return state;}bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
 int send(uint8_t *,size_t)override{++sends;state=0;return 0;}bool completion(int &)override{return false;}
};
struct Host:mt::Companion::Host {
 std::vector<meshtastic_FromRadio> output;unsigned previews=0;bool ready()const override{return true;}int save(const mt::Config&)override{return -ENOTSUP;}
 bool send(const uint8_t *p,size_t n)override{assert(n>=4);meshtastic_FromRadio out{};auto s=pb_istream_from_buffer(p+4,n-4);assert(pb_decode(&s,meshtastic_FromRadio_fields,&out));output.push_back(out);return true;}
 void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool)override{++previews;}
};
struct PolicyHost final:Host {
 mt::Service &service;Radio &radio;Journal &journal;Memory &backend;bool blocked=false;Serial serial{};
 mt::Companion *clients[2]{};unsigned forgotten=0;
 PolicyHost(mt::Service &s,Radio &r,Journal &j,Memory &b):service(s),radio(r),journal(j),backend(b){}
 static void removed(uint32_t id,void *context){auto &h=*static_cast<PolicyHost *>(context);++h.forgotten;for(auto *c:h.clients)if(c)c->forgetPeer(id);}
 int editNode(uint32_t id,Cache::Edit op,uint32_t now)override{return mt::NodeAdmin::edit(service,radio,journal,backend,id,op,now,blocked,removed,this);}
 Serial serialStatus()const override{return serial;}
};
static void admin(mt::Companion &c,pb_size_t tag,uint32_t id,uint32_t request=1000){
 meshtastic_AdminMessage a{};a.which_payload_variant=tag;
 switch(tag){
 case meshtastic_AdminMessage_factory_reset_config_tag:a.factory_reset_config=int32_t(id);break;
 case meshtastic_AdminMessage_factory_reset_device_tag:a.factory_reset_device=int32_t(id);break;
 case meshtastic_AdminMessage_nodedb_reset_tag:a.nodedb_reset=(id!=0);break;
 case meshtastic_AdminMessage_delete_file_request_tag:strcpy(a.delete_file_request,"/static/rangetest.csv");break;
 case meshtastic_AdminMessage_set_favorite_node_tag:a.set_favorite_node=id;break;
 case meshtastic_AdminMessage_remove_favorite_node_tag:a.remove_favorite_node=id;break;
 case meshtastic_AdminMessage_set_ignored_node_tag:a.set_ignored_node=id;break;
 case meshtastic_AdminMessage_remove_ignored_node_tag:a.remove_ignored_node=id;break;
 case meshtastic_AdminMessage_remove_by_nodenum_tag:a.remove_by_nodenum=id;break;
 case meshtastic_AdminMessage_get_device_connection_status_request_tag:a.get_device_connection_status_request=true;break;
 default:assert(false);
 }
 meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=t.packet;
 p.to=Self;p.id=request;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
 auto encoded=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&encoded,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=encoded.bytes_written;
 uint8_t bytes[512];encoded=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&encoded,meshtastic_ToRadio_fields,&t));c.packet(bytes,encoded.bytes_written,200,mt::Companion::WorkspaceProof{true});
}
static meshtastic_Routing_Error adminError(const Host &h){
 const auto &p=h.output.back().packet;assert(p.decoded.portnum==meshtastic_PortNum_ROUTING_APP);
 meshtastic_Routing r{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&stream,meshtastic_Routing_fields,&r));return r.error_reason;
}
static void handshake(mt::Companion &c){meshtastic_ToRadio t{};t.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;t.want_config_id=7;uint8_t b[512];auto out=pb_ostream_from_buffer(b,sizeof(b));assert(pb_encode(&out,meshtastic_ToRadio_fields,&t));c.packet(b,out.bytes_written,1,mt::Companion::WorkspaceProof{true});}
static Bytes frame(mt::Service &s,uint32_t id,unsigned keyByte=0x42,uint32_t peer=Peer){
 meshtastic_User u{};strcpy(u.id,"!deadbeef");strcpy(u.long_name,"Synthetic peer");strcpy(u.short_name,"SYN");
 const uint8_t mac[6]={0xda,0x7a,0xee,0x13,0x66,0x91};memcpy(u.macaddr,mac,6);u.hw_model=static_cast<meshtastic_HardwareModel>(127);u.role=meshtastic_Config_DeviceConfig_Role_CLIENT;
 u.public_key.size=32;memset(u.public_key.bytes,keyByte,32);u.has_is_unmessagable=true;
 uint8_t body[233];auto out=pb_ostream_from_buffer(body,sizeof(body));assert(pb_encode(&out,meshtastic_User_fields,&u));
 Bytes wire(255);size_t length=0;assert(s.codec.encodeZeroHopNodeInfo(peer,id,body,out.bytes_written,wire.data(),wire.size(),length));wire.resize(length);return wire;
}
int main(){
 {Radio radio;mt::test::KnownChannelFixtureClock clock;mt::Service service(radio,true,Self,127,true,false);
  clock.bind(service);mt::companion::CompanionWorkspace workspace;
  struct Controls:Host{
    unsigned resets=0,devices=0,databases=0,files=0;bool keep=false,storageReady=true;
    int resetResult=0;
    bool ready()const override{return storageReady;}
    int requestFactoryConfigReset()override{++resets;storageReady=false;return resetResult;}
    int requestFactoryDeviceReset()override{++devices;storageReady=false;return resetResult;}
    int resetNodeDb(bool k,uint32_t)override{++databases;keep=k;return 0;}
    int removeAppFile(const char *file)override{assert(!strcmp(file,"/static/rangetest.csv"));++files;return 0;}
  } host;
  mt::Companion c(service,host,127,workspace);handshake(c);host.output.clear();
  auto oneRoutingAck=[&host](uint32_t request){
    return std::count_if(host.output.begin(),host.output.end(),[request](const meshtastic_FromRadio &row){
      return row.which_payload_variant==meshtastic_FromRadio_packet_tag&&
        row.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&row.packet.decoded.request_id==request;
    })==1;
  };
  admin(c,meshtastic_AdminMessage_factory_reset_config_tag,1,8001);assert(adminError(host)==meshtastic_Routing_Error_NONE&&host.resets==1);
  assert(!host.storageReady&&oneRoutingAck(8001));
  host.storageReady=true;
  host.output.clear();admin(c,meshtastic_AdminMessage_nodedb_reset_tag,1,8002);assert(adminError(host)==meshtastic_Routing_Error_NONE&&host.databases==1&&host.keep);
  host.output.clear();admin(c,meshtastic_AdminMessage_delete_file_request_tag,0,8003);assert(adminError(host)==meshtastic_Routing_Error_NONE&&host.files==1);
  host.output.clear();admin(c,meshtastic_AdminMessage_factory_reset_device_tag,1,8005);assert(adminError(host)==meshtastic_Routing_Error_NONE&&host.devices==1);
  assert(!host.storageReady&&oneRoutingAck(8005));
  host.storageReady=true;host.resetResult=-EIO;
  host.output.clear();admin(c,meshtastic_AdminMessage_factory_reset_device_tag,1,8007);
  assert(!host.storageReady&&adminError(host)==meshtastic_Routing_Error_NO_INTERFACE&&host.devices==2);
  host.storageReady=true;
  auto config=service.config;assert(config.setSecurityControls(true,true)&&service.apply(config));
  host.output.clear();admin(c,meshtastic_AdminMessage_factory_reset_config_tag,1,8004);
  assert(adminError(host)==meshtastic_Routing_Error_NOT_AUTHORIZED&&host.resets==1);
  host.output.clear();admin(c,meshtastic_AdminMessage_factory_reset_device_tag,1,8006);
  assert(adminError(host)==meshtastic_Routing_Error_NOT_AUTHORIZED&&host.devices==2);
  PASS("encoded local factory settings, NodeDB and app-file requests reach their owners; managed mode denies local reset");}

 {Memory b;Cache c;Journal j;
  assert(!j.load(b,c,Self)&&c.accept(entry(),Self,4,0)==0);
  assert(j.flush(b,c,100,true,true)==1&&b.files["mtbase/nodes0"].size()==Journal::RecordSize);
  c.setRouteHint(Peer,0x33);assert(c.routeHint(Peer)==0x33&&j.flush(b,c,101,true,true)==1);
  const Bytes withHint=b.files["mtbase/nodes1"];
  assert(withHint[3]==6&&withHint.size()==Journal::HintRecordSize);
  Cache restarted;Journal after;assert(!after.load(b,restarted,Self)&&restarted.routeHint(Peer)==0x33);
  assert(after.edit(b,restarted,Peer,Cache::Edit::Remove,102,true)==0&&restarted.routeHint(Peer)==0);
  for(unsigned cut=0;cut<withHint.size();++cut){
    Memory torn;torn.files["mtbase/nodes1"]={withHint.begin(),withHint.begin()+cut};
    Journal blank;Cache empty;assert(blank.load(torn,empty,Self)<0&&!empty.ready()&&!empty.routeHint(Peer));
  }
  assert(c.accept(entry(Peer,"Changed"),Self,-8,1)==0&&c.routeHint(Peer)==0x33);
  c.setRouteHint(Peer,0);assert(j.flush(b,c,104,true,true)==1&&b.files["mtbase/nodes0"].size()==Journal::RecordSize);
  PASS("next-hop hints migrate from old records, survive checked readback, reject every partial write and disappear on removal");}

 {Memory b;Cache c;Journal j;assert(j.state()==Journal::State::Unloaded&&j.activeSlot()==-1);
  assert(!j.load(b,c,Self)&&j.ready()&&c.ready()&&!c.count()&&!b.writes&&j.activeSlot()==-1);
  assert(c.accept(entry(),Self,4,0)==0&&c.count()==1&&c.dirty());j.observe(c,100);assert(!j.due(60099)&&j.due(60100));
  const int reads=b.reads;assert(j.flush(b,c,60100,false)==-EAGAIN&&!b.writes);assert(j.flush(b,c,60100,false,true)==-EAGAIN&&!b.writes&&b.reads==reads);
  assert(j.flush(b,c,60100,true)==1&&b.writes==1&&!c.dirty()&&j.activeSlot()==0);assert(j.flush(b,c,60101,true)==0&&b.writes==1);
  assert(c.accept(entry(),Self,-8,3)==0&&!c.dirty());assert(j.flush(b,c,900000,true)==0&&b.writes==1);
  assert(c.accept(entry(Peer,"Changed name"),Self,7,1)==0);j.observe(c,60200);assert(!j.due(660099)&&j.due(660100));
  assert(j.flush(b,c,660100,true)==1&&b.writes==2);
  PASS("coalescing, content-only dirty state, ten-minute wear limit and explicit quiescent gate");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,9,2)==0);assert(j.flush(b,c,0,true,true)==1);
  Cache restored;Journal reboot;assert(!reboot.load(b,restored,Self)&&restored.count()==1&&!restored.dirty());
  assert(!restored.get(0)->heard&&!restored.get(0)->snr&&!restored.get(0)->hops);
  assert(restored.accept(entry(Peer,"Replacement",32,0x41),Self,1,0)==-EACCES);
  assert(restored.accept(entry(Peer,"Removed",0),Self,1,0)==-EACCES);
  assert(restored.accept(entry(Peer,"Same key new name"),Self,1,0)==0);
  Cache wrong;Journal copied;assert(copied.load(b,wrong,Peer)==-EILSEQ&&!wrong.ready());
  PASS("reboot retains peer key guard and identities; link metrics unknown; local identity excluded");}
 {static_assert(Cache::Entry::Size==92&&Journal::RecordSize==1492,"MQTT provenance must not change the durable NodeDB format");
  Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,1,0)==0);
  assert(j.flush(b,c,0,true,true)==1&&b.writes==1);const Bytes durable=b.files["mtbase/nodes0"];
  assert(c.observeReceivedPacket(Peer,1700000000,true));
  assert(c.get(0)->lastHeard()==1700000000&&c.get(0)->viaMqtt&&!c.dirty());
  assert(!memcmp(c.get(0)->bytes,durable.data()+Journal::HeaderSize,Cache::Entry::Size));
  assert(j.flush(b,c,1,true,true)==0&&b.writes==1&&b.files["mtbase/nodes0"]==durable);
  assert(c.observeReceivedPacket(Peer,0,false));
  assert(c.get(0)->lastHeard()==1700000000&&!c.get(0)->viaMqtt);
  assert(!c.observeReceivedPacket(Peer+1,20,true)&&c.count()==1);
  assert(c.edit(Peer,Self,Cache::Edit::Ignore)==1);const Cache ignored=c;
  assert(!c.observeReceivedPacket(Peer,30,true)&&!memcmp(&ignored,&c,sizeof(c)));
  assert(c.edit(Peer+1,Self,Cache::Edit::Ignore)==1);const Cache placeholder=c;
  assert(c.get(1)->placeholder()&&!c.observeReceivedPacket(Peer+1,40,true)&&!memcmp(&placeholder,&c,sizeof(c)));
  Cache restored;Journal reboot;assert(!reboot.load(b,restored,Self)&&restored.count()==1);
  assert(!restored.get(0)->viaMqtt&&restored.get(0)->lastHeard()==0&&!restored.dirty());
  b.files["mtbase/nodes0"][100]^=1;Cache frozen;Journal damaged;
  assert(damaged.load(b,frozen,Self)==-EILSEQ&&!frozen.ready());const Cache beforeFrozen=frozen;
  assert(!frozen.observeReceivedPacket(Peer,50,true)&&!memcmp(&beforeFrozen,&frozen,sizeof(frozen)));
  PASS("latest packet origin is volatile and guarded; zero UTC, unknown/ignored/placeholder/frozen paths preserve trust and the 1492-byte journal");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
  for(unsigned i=0;i<20;++i)assert(c.accept(entry(0x40000000+i),Self,1,0)>=0);
  assert(c.count()==16&&c.find(0x40000000)<0&&c.find(0x40000004)>=0);
  assert(j.flush(b,c,1,true,true)==1);Cache r;Journal k;assert(!k.load(b,r,Self));
  assert(r.accept(entry(0x50000000),Self,1,0)==4&&r.find(0x40000004)<0&&r.count()==16);
  PASS("capacity16, finite eviction and eviction cursor preserved across reboot");}
 {Memory base;Cache c;Journal j;assert(!j.load(base,c,Self));assert(c.accept(entry(Peer,"No key yet",0),Self,1,0)==0);assert(j.flush(base,c,0,true,true)==1);
  const auto first=base.files["mtbase/nodes0"];
  for(unsigned cut=0;cut<Journal::RecordSize;++cut){Memory b=base;Cache active;Journal a;assert(!a.load(b,active,Self));
   assert(active.accept(entry(),Self,1,0)==0);b.cut=int(cut);assert(a.flush(b,active,1,true,true)==-EIO&&!active.ready());
   b.cut=-1;Cache r;Journal reboot;assert(reboot.load(b,r,Self)==-EILSEQ&&reboot.state()==Journal::State::RecoveredFrozen&&!r.ready());
   assert(r.count()==1&&r.get(0)->bytes[Cache::Entry::KeyLength]==0);
   assert(r.accept(entry(Peer,"Downgrade",0),Self,0,0)==-EACCES&&r.accept(entry(0x77777777),Self,0,0)==-EACCES);
   assert(reboot.flush(b,r,900000,true,true)==-EACCES&&b.files["mtbase/nodes0"]==first);
  }PASS("all1492 torn-write cut points freeze fallback; first-key and absent-peer guards cannot reopen");}
 {Memory base;Cache c;Journal j;assert(!j.load(base,c,Self));assert(c.accept(entry(Peer,"No key",0),Self,1,0)==0);assert(j.flush(base,c,0,true,true)==1);
  assert(c.accept(entry(),Self,1,0)==0&&c.accept(entry(0x77777777),Self,1,0)>=0);assert(j.flush(base,c,1,true,true)==1);
  for(unsigned byte=0;byte<Journal::RecordSize;++byte){auto b=base;b.files["mtbase/nodes1"][byte]^=0x80;Cache r;Journal a;
   assert(a.load(b,r,Self)==-EILSEQ&&!r.ready()&&r.count()==1);
   assert(r.accept(entry(),Self,0,0)==-EACCES&&r.accept(entry(0x77777777),Self,0,0)==-EACCES);
  }PASS("every newest-record byte corruption freezes old-key and completely-new-peer fallback paths");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(Peer,"Older name"),Self,1,0)==0);assert(j.flush(b,c,0,true,true)==1);
  assert(c.accept(entry(Peer,"Newest name"),Self,1,0)==0);assert(j.flush(b,c,1,true,true)==1);b.files["mtbase/nodes0"][100]^=1;
  Cache r;Journal a;assert(a.load(b,r,Self)==-EILSEQ&&a.state()==Journal::State::RecoveredFrozen&&!r.ready());
  assert(!strcmp(reinterpret_cast<const char*>(r.get(0)->bytes+Cache::Entry::Name),"Newest name"));
  assert(r.accept(entry(Peer,"Later observation"),Self,1,0)==-EACCES);
  PASS("corrupt older-present slot also freezes updates while retaining newest valid display data");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,1,0)==0);assert(j.flush(b,c,0,true,true)==1);
  assert(c.accept(entry(Peer,"After commit error"),Self,1,0)==0);b.committedError=true;assert(j.flush(b,c,1,true,true)==-EIO&&!c.ready()&&!j.ready());
  b.committedError=false;assert(j.flush(b,c,2,true,true)==-EACCES);Cache r;Journal a;assert(!a.load(b,r,Self));assert(!strcmp(reinterpret_cast<const char*>(r.get(0)->bytes+Cache::Entry::Name),"After commit error"));
  assert(r.accept(entry(Peer,"Corrupted readback"),Self,1,0)==0);b.corruptReadback=true;assert(a.flush(b,r,3,true,true)==-EIO&&!r.ready());
  PASS("full commit with error and corrupt readback freeze all later cache writes without false success");}
 {Memory b;Cache c;Journal j;b.readError=-EIO;assert(j.load(b,c,Self)==-EIO&&!c.ready());
  b.readError=0;b.files["mtbase/nodes0"]=Bytes(Journal::RecordSize+1,0);assert(j.load(b,c,Self)==-EILSEQ&&!c.ready());
  b.files.clear();assert(j.load(b,c,1)==-EINVAL&&!c.ready());assert(!j.load(b,c,Self));
  auto invalid=entry();invalid.bytes[91]=1;assert(c.accept(invalid,Self,0,0)==-EINVAL);
  invalid=entry();invalid.bytes[Cache::Entry::KeyLength]=33;assert(c.accept(invalid,Self,0,0)==-EINVAL);
  invalid=entry();memset(invalid.bytes+Cache::Entry::Name,'x',40);assert(c.accept(invalid,Self,0,0)==-EINVAL);
  assert(c.accept(entry(Self),Self,0,0)==-EINVAL&&!c.count());
  PASS("I/O, oversized or invalid records never become fresh cache; runtime input bounds reject");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,1,0)==0);assert(j.flush(b,c,0,true,true)==1);
  const auto original=b.files["mtbase/nodes0"];
  for(unsigned where:{8u,9u,10u,11u,15u,16u+Cache::Entry::Flags,16u+Cache::Entry::KeyLength,16u+91u}){
   b.files["mtbase/nodes0"]=original;b.files["mtbase/nodes0"][where]=255;seal(b.files["mtbase/nodes0"]);Cache r;Journal a;assert(a.load(b,r,Self)==-EILSEQ&&!r.ready());}
  b.files["mtbase/nodes0"]=original;auto &raw=b.files["mtbase/nodes0"];raw[8]=2;memcpy(raw.data()+16+Cache::Entry::Size,raw.data()+16,Cache::Entry::Size);seal(raw);Cache r;Journal a;assert(a.load(b,r,Self)==-EILSEQ&&!r.ready());
  PASS("valid-CRC malformed counts/cursors/header/flags/keys and duplicate peer IDs reject");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,1,0)==0);assert(j.flush(b,c,0,true,true)==1);
  assert(c.accept(entry(Peer,"New generation"),Self,1,0)==0);assert(j.flush(b,c,1,true,true)==1);
  generation(b.files["mtbase/nodes0"],UINT32_MAX);generation(b.files["mtbase/nodes1"],0);assert(!j.load(b,c,Self)&&j.generation()==0&&j.activeSlot()==1);
  generation(b.files["mtbase/nodes0"],0);assert(j.load(b,c,Self)==-EINVAL&&!c.ready());
  generation(b.files["mtbase/nodes0"],0x80000000u);assert(j.load(b,c,Self)==-EINVAL&&!c.ready());
  PASS("generation ordering wraps; ambiguous and conflicting versions freeze");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,1,0)==0);j.observe(c,0xfffffff0u);
  assert(!j.due(0xfffffff0u+59999u)&&j.due(0xfffffff0u+60000u));assert(j.flush(b,c,0xfffffff0u+60000u,true)==1);
  assert(c.accept(entry(Peer,"After wrap"),Self,1,0)==0);j.observe(c,60000);assert(!j.due(659983)&&j.due(659984));
  PASS("deferred save scheduling remains bounded through uptime wrap");}
 {Radio radio;mt::Service s(radio,false,Self,81,true);auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config)&&s.start(false,0));
  Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion a(s,usb,81,workspace,0),z(s,ble,81,workspace,1);handshake(a);handshake(z);usb.output.clear();ble.output.clear();
  auto f=frame(s,100);a.receive(f.data(),f.size(),-90,4,200,mt::Companion::WorkspaceProof{true});z.receive(f.data(),f.size(),-90,4,200,mt::Companion::WorkspaceProof{true});
  assert(s.nodes.count()==1&&usb.output.size()==2&&ble.output.size()==2&&usb.previews==1&&ble.previews==1);
  assert(usb.output[0].node_info.num==Peer&&!strcmp(usb.output[0].node_info.user.id,"!87654321"));
  auto wrong=frame(s,101,0x41);a.receive(wrong.data(),wrong.size(),-90,4,201,mt::Companion::WorkspaceProof{true});z.receive(wrong.data(),wrong.size(),-90,4,201,mt::Companion::WorkspaceProof{true});
  assert(usb.output.size()==2&&ble.output.size()==2);s.stop();assert(!s.receiving()&&!s.active()&&!s.faulted());
  Memory b;Journal j;Cache initial;assert(!j.load(b,initial,Self));
  assert(j.flush(b,s.nodes,1,true,true)==1);const uint8_t mac[6]={0xda,0x7a,0xee,0x13,0x66,0x91};
  const auto &raw=b.files["mtbase/nodes0"];assert(std::search(raw.begin(),raw.end(),mac,mac+6)==raw.end());
  Radio newRadio;mt::Service restored(newRadio,false,Self,81,true);Journal reboot;assert(!reboot.load(b,restored.nodes,Self));
  Host h;mt::companion::CompanionWorkspace workspaceRestored;mt::Companion c(restored,h,81,workspaceRestored);handshake(c);bool found=false;
  for(const auto &out:h.output)if(out.which_payload_variant==meshtastic_FromRadio_node_info_tag&&out.node_info.num==Peer){
   found=true;assert(out.node_info.user.public_key.size==32&&!out.node_info.has_position&&!out.node_info.has_device_metrics&&!out.node_info.heard_on_current_lora&&!out.node_info.has_hops_away&&!out.node_info.last_heard);
   for(auto byte:out.node_info.user.macaddr)assert(!byte);
  }
  assert(found&&!restored.receiving()&&!restored.endpointArmed()&&!newRadio.sends);
  b.files["mtbase/nodes1"]=Bytes{1,2,3};assert(reboot.load(b,restored.nodes,Self)==-EILSEQ);
  assert(restored.apply(config)&&!restored.start(false,0));const auto before=h.output.size();
  f=frame(restored,200);c.receive(f.data(),f.size(),-80,3,500,mt::Companion::WorkspaceProof{true});assert(h.output.size()==before&&!h.previews);
  PASS("USB/BLE each receive NodeInfo and raw packet; shared guard, normalized ID and reboot handshake survive without MAC/position/RF state");}
 {Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal1;mt::Service s(radio,true,Self,81,true);knownChannelLocal1.bind(s);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config));assert(s.nodes.accept(entry(),Self,1,0)==0);
  const int reads=b.reads;
  auto flush=[&](){return j.flush(b,s.nodes,900000,s.storageQuiescent()&&radio.ready()&&radio.health()==0,true);};
  auto deferred=[&](){assert(flush()==-EAGAIN&&b.reads==reads&&!b.writes&&s.nodes.dirty());};
  assert(s.storageQuiescent());assert(s.start(false,0));deferred(); // Healthy RX must never be stopped to save.
  assert(s.armEndpoint(true));deferred();assert(s.endpointArmed()&&s.receiving());
  assert(!s.submitNodeInfo(0,300,0xffffffffu,0,false,false,100));deferred();
  s.poll(300,700);assert(s.active()&&radio.sends==1&&radio.health()==0);deferred(); // Native health zero includes TX.
  s.stop();assert(s.storageQuiescent());radio.available=false;deferred();radio.available=true;
  radio.state=1;deferred();radio.state=-EIO;deferred();radio.state=0;
  assert(flush()==1&&b.writes==1&&!s.nodes.dirty());
  assert(s.nodes.accept(entry(Peer,"After good save"),Self,1,0)==0);assert(s.start(false,0));
  radio.stopError=-EIO;s.stop();radio.state=0; // A cleared flag or healthy-looking native status cannot clear the fault.
  assert(!s.receiving()&&!s.active()&&s.faulted()&&!s.storageQuiescent());const int after=b.reads;
  assert(flush()==-EAGAIN&&b.writes==1&&b.reads==after&&s.nodes.dirty());
  PASS("real Service quiescence and native readiness gate journal I/O during RX, arm, queued/active NodeInfo TX and failed stop");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));assert(c.accept(entry(),Self,1,0)==0);assert(j.flush(b,c,0,true,true)==1);
  auto &v1=b.files["mtbase/nodes0"];v1[3]=1;seal(v1);const Bytes original=v1;
  Cache loaded;Journal migration;assert(!migration.load(b,loaded,Self)&&loaded.ready()&&!loaded.get(0)->favorite()&&b.writes==1);
  assert(migration.edit(b,loaded,Peer,Cache::Edit::Favorite,1,true)==0&&loaded.get(0)->favorite());
  assert(b.files["mtbase/nodes0"]==original&&b.files["mtbase/nodes1"][3]==3);
  Cache reboot;Journal check;assert(!check.load(b,reboot,Self)&&reboot.get(0)->favorite()&&reboot.get(0)->bytes[Cache::Entry::KeyLength]==32);
  assert(!memcmp(reboot.get(0)->bytes+Cache::Entry::Key,entry().bytes+Cache::Entry::Key,32));
  PASS("v1 loads without write; first verified v3 policy commit preserves exact original slot and peer key");}
 {Memory base;Cache cache;Journal j;assert(!j.load(base,cache,Self));assert(cache.accept(entry(),Self,1,0)==0&&j.flush(base,cache,0,true,true)==1);
  base.files["mtbase/nodes0"][3]=1;seal(base.files["mtbase/nodes0"]);const Bytes original=base.files["mtbase/nodes0"];
  for(unsigned cut=0;cut<Journal::RecordSize;++cut){auto b=base;Cache c;Journal a;assert(!a.load(b,c,Self));b.cut=int(cut);
   assert(a.edit(b,c,Peer,Cache::Edit::Ignore,1,true)==-EIO&&!c.ready()&&!c.ignored(Peer));
   assert(b.files["mtbase/nodes0"]==original);b.cut=-1;Cache recovered;Journal r;
   assert(r.load(b,recovered,Self)==-EILSEQ&&!recovered.ready()&&!recovered.ignored(Peer)&&recovered.count()==1);
  }PASS("all1492 migration cut points retain old v1 policy/key; torn v3 freezes fallback and never publishes ignore");}
 {Cache c;assert(c.accept(entry(),Self,1,0)==0);assert(c.edit(Peer,Self,Cache::Edit::Favorite)==1);
  assert(c.edit(Peer,Self,Cache::Edit::ToggleMute)==1&&c.muted(Peer)&&c.protectedCount()==1);
  assert(c.accept(entry(Peer,"Updated over RF"),Self,2,0)==0&&c.get(0)->favorite()&&c.muted(Peer));
  auto forged=entry();forged.bytes[Cache::Entry::LocalFlags]=Cache::Entry::Ignored;assert(c.accept(forged,Self,0,0)==-EINVAL);
  assert(c.edit(Peer,Self,Cache::Edit::Ignore)==1&&c.permits(entry(),Self,0)==-EACCES);
  assert(c.edit(Peer,Self,Cache::Edit::Unignore)==1&&c.get(0)->favorite());
  for(uint32_t id:{0u,1u,Self,0xfffffffeu,0xffffffffu})assert(c.edit(id,Self,Cache::Edit::Ignore)==-EINVAL);
  assert(c.edit(Peer+1,Self,Cache::Edit::Favorite)==0&&c.edit(Peer+1,Self,Cache::Edit::Unfavorite)==0&&c.edit(Peer+1,Self,Cache::Edit::Unignore)==-ENOENT&&c.edit(Peer+1,Self,Cache::Edit::ToggleMute)==-ENOENT);
  assert(c.edit(Peer+1,Self,Cache::Edit::Remove)==0);
  PASS("favorite/mute policy survives RF updates; RF cannot set local flags; mute is nonprotective; absent favorite is inert while unignore/mute reject");}
 {Cache c;for(unsigned i=0;i<Cache::ProtectedLimit;++i)assert(c.edit(0x60000000+i,Self,Cache::Edit::Ignore)==1);
  assert(c.count()==14&&c.protectedCount()==14&&c.get(0)->placeholder());
  assert(c.edit(0x70000000,Self,Cache::Edit::Ignore)==-ENOSPC);
  for(unsigned i=0;i<100;++i)assert(c.accept(entry(0x70000000+i),Self,1,0)>=0);
  assert(c.count()==16&&c.protectedCount()==14);
  for(unsigned i=0;i<14;++i)assert(c.ignored(0x60000000+i));
  assert(c.edit(0x60000000,Self,Cache::Edit::Favorite)==1&&c.protectedCount()==14);
  assert(c.edit(0x60000000,Self,Cache::Edit::Unignore)==1&&c.get(unsigned(c.find(0x60000000)))->favorite());
  assert(c.edit(0x60000000,Self,Cache::Edit::Unfavorite)==1&&c.protectedCount()==13);
  assert(c.accept(entry(0x60000000),Self,2,0)>=0&&!c.get(unsigned(c.find(0x60000000)))->placeholder());
  PASS("14 favorite/ignored protected entries survive bounded churn; two slots remain; placeholder gains only received identity");}
 {Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal2;mt::Service s(radio,true,Self,127,true,false);knownChannelLocal2.bind(s);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  PolicyHost h(s,radio,j,b);mt::companion::CompanionWorkspace workspace;mt::Companion c(s,h,127,workspace);assert(s.nodes.accept(entry(),Self,1,0)==0);handshake(c);h.output.clear();
  admin(c,meshtastic_AdminMessage_set_favorite_node_tag,Peer);assert(adminError(h)==0&&s.nodes.get(0)->favorite()&&h.output[h.output.size()-2].node_info.is_favorite);
  const int written=b.writes;admin(c,meshtastic_AdminMessage_set_favorite_node_tag,Peer);assert(adminError(h)==0&&b.writes==written);
  admin(c,meshtastic_AdminMessage_set_ignored_node_tag,Peer+1);assert(adminError(h)==0&&s.nodes.ignored(Peer+1));
  assert(h.output[h.output.size()-2].node_info.is_ignored&&!h.output[h.output.size()-2].node_info.has_user);
  admin(c,meshtastic_AdminMessage_remove_favorite_node_tag,Peer);assert(adminError(h)==0&&!s.nodes.get(0)->favorite());
  admin(c,meshtastic_AdminMessage_remove_ignored_node_tag,Peer+1);assert(adminError(h)==0&&!s.nodes.ignored(Peer+1));
  admin(c,meshtastic_AdminMessage_remove_by_nodenum_tag,Peer);assert(adminError(h)==0&&s.nodes.find(Peer)<0);
  const int deleted=b.writes;admin(c,meshtastic_AdminMessage_remove_by_nodenum_tag,Peer);assert(adminError(h)==0&&b.writes==deleted);
  admin(c,meshtastic_AdminMessage_set_favorite_node_tag,Peer);assert(adminError(h)==meshtastic_Routing_Error_NONE&&b.writes==deleted&&s.nodes.find(Peer)<0);
  h.output.clear();handshake(c);assert(h.output.front().my_info.nodedb_count==2);
  for(const auto &r:h.output)if(r.which_payload_variant==meshtastic_FromRadio_node_info_tag&&r.node_info.num!=Self)assert(r.node_info.num==Peer+1&&!r.node_info.has_user);
  assert(radio.sends==0&&!s.receiving()&&!s.endpointArmed());
  PASS("five local admin tags commit and ACK; unchanged/deleted idempotence; truthful favorite/ignored/placeholder handshake");}
 {Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal3;mt::Service s(radio,true,Self,127,true,false);knownChannelLocal3.bind(s);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config));assert(s.nodes.accept(entry(),Self,1,0)==0);
  assert(s.start(false,0)&&s.armEndpoint(true));int writes=b.writes;
  PolicyHost host(s,radio,j,b);mt::companion::CompanionWorkspace workspace;mt::Companion c(s,host,127,workspace);handshake(c);host.output.clear();
  admin(c,meshtastic_AdminMessage_set_favorite_node_tag,Peer);
  assert(adminError(host)==meshtastic_Routing_Error_NONE&&b.writes==writes+1&&s.endpointArmed()&&s.receiving());
  assert(s.nodes.get(0)->favorite());writes=b.writes;
  assert(s.armEndpoint(false));assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Favorite,1,true)==-EBUSY&&s.receiving()&&b.writes==writes);
  assert(mt::NodeAdmin::edit(s,radio,j,b,Self,Cache::Edit::Ignore,1)==-EINVAL&&s.receiving()&&b.writes==writes);
  assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Favorite,2)==0&&s.receiving()&&!s.endpointArmed()&&b.writes==writes);
  radio.stopError=-EIO;writes=b.writes;
  assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Ignore,3)==-EIO&&b.writes==writes&&s.faulted()&&s.receiving()&&!s.nodes.ignored(Peer));
  PASS("actual NodeAdmin admits armed edit, preserves blocked/invalid requests, and faults without falsely clearing live RX on uncertain stop");}
 for(bool uncertain:{false,true}){Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal4;mt::Service s(radio,true,Self,127,true,false);knownChannelLocal4.bind(s);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config));assert(s.nodes.accept(entry(),Self,1,0)==0&&j.flush(b,s.nodes,0,true,true)==1);
  assert(s.start(false,0));if(uncertain)b.committedError=true;else b.writeError=-EIO;
  assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Ignore,1)==-EIO);
  assert(!s.nodes.ready()&&!j.ready()&&!s.nodes.ignored(Peer)&&!s.receiving()&&!s.endpointArmed()&&!s.start(false,2));
  b.committedError=false;b.writeError=0;Cache recovered;Journal reboot;assert(!reboot.load(b,recovered,Self)&&recovered.ignored(Peer)==uncertain);
  PASS("candidate commit failure freezes unchanged runtime cache and RF off; reboot truthfully selects actual durable generation");}
 {Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal5;mt::Service s(radio,true,Self,127,true,false);knownChannelLocal5.bind(s);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config));assert(s.nodes.accept(entry(),Self,1,0)==0&&j.flush(b,s.nodes,0,true,true)==1);
  assert(s.start(false,0));b.corruptReadback=true;assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Favorite,1)==-EIO);
  assert(!s.nodes.get(0)->favorite()&&!s.nodes.ready()&&!s.receiving()&&!s.start(false,1));
  PASS("corrupt policy readback never publishes new favorite and cannot resume RF");}
 {Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal6;mt::Service s(radio,true,Self,127,true,false);knownChannelLocal6.bind(s);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config));
  PolicyHost usb(s,radio,j,b),ble(s,radio,j,b);mt::companion::CompanionWorkspace workspace;mt::Companion c(s,usb,127,workspace),z(s,ble,127,workspace,1);handshake(c);handshake(z);usb.output.clear();ble.output.clear();
  assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Ignore,1)==0);assert(s.start(false,2)&&s.armEndpoint(true));
  meshtastic_Data text{};text.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;text.has_bitfield=true;text.payload.size=1;text.payload.bytes[0]='x';
  uint8_t payload[64];auto enc=pb_ostream_from_buffer(payload,sizeof(payload));assert(pb_encode(&enc,meshtastic_Data_fields,&text));
  Bytes direct(255);size_t length=0;assert(s.codec.encodeZeroHopData(Peer,Self,902,payload,enc.bytes_written,direct.data(),direct.size(),length));direct.resize(length);direct[12]=8;
  s.rx(direct.data(),direct.size(),2,99,123);c.receive(direct.data(),direct.size(),-90,2,99,mt::Companion::WorkspaceProof{true});z.receive(direct.data(),direct.size(),-90,2,99,mt::Companion::WorkspaceProof{true});
  auto f=frame(s,900);s.rx(f.data(),f.size(),2,100,234);c.receive(f.data(),f.size(),-90,2,100,mt::Companion::WorkspaceProof{true});z.receive(f.data(),f.size(),-90,2,100,mt::Companion::WorkspaceProof{true});s.poll(1000,567);
  assert(radio.sends==0&&usb.output.empty()&&ble.output.empty()&&!usb.previews&&!ble.previews&&s.nodes.get(0)->placeholder());
  assert(s.armEndpoint(false));assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Unignore,1001)==0);
  s.rx(f.data(),f.size(),2,1002,234);c.receive(f.data(),f.size(),-90,2,1002,mt::Companion::WorkspaceProof{true});z.receive(f.data(),f.size(),-90,2,1002,mt::Companion::WorkspaceProof{true});
  assert(!s.nodes.get(0)->placeholder()&&usb.output.size()==2&&ble.output.size()==2&&usb.previews==1&&ble.previews==1);
  assert(mt::NodeAdmin::edit(s,radio,j,b,Peer,Cache::Edit::Favorite,1003)==0);
  const auto body=frame(s,901);bench::RouteMetadata meta;assert(s.codec.decode(body.data(),body.size(),meta));
  assert(mt::NodeUser::accepted(s.nodes,Peer,meta.payload,meta.payloadLength));
  assert(s.armEndpoint(true));s.rx(direct.data(),direct.size(),2,2000,789);c.receive(direct.data(),direct.size(),-90,2,2000,mt::Companion::WorkspaceProof{true});z.receive(direct.data(),direct.size(),-90,2,2000,mt::Companion::WorkspaceProof{true});s.poll(3000,456);
  assert(radio.sends==1&&usb.previews==2&&ble.previews==2&&usb.output.size()==3&&ble.output.size()==3);
  PASS("ignored RF cannot generate ACK/cache/UI/USB/BLE or poison duplicate history; same text ACKed after unignore; favorites preserve response match");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));for(unsigned i=0;i<16;++i)assert(c.accept(entry(0x70000000+i),Self,1,0)>=0);assert(j.flush(b,c,0,true,true)==1);
  const Bytes original=b.files["mtbase/nodes0"];
  for(unsigned mode=0;mode<4;++mode){auto &raw=b.files["mtbase/nodes0"];raw=original;
   if(mode==0){raw[3]=1;raw[16+Cache::Entry::LocalFlags]=Cache::Entry::Favorite;}
   if(mode==1)raw[16+Cache::Entry::LocalFlags]=Cache::Entry::Placeholder;
   if(mode==2)for(unsigned i=0;i<15;++i)raw[16+i*Cache::Entry::Size+Cache::Entry::LocalFlags]=Cache::Entry::Favorite;
   if(mode==3){raw[3]=2;raw[16+Cache::Entry::LocalFlags]=Cache::Entry::Muted;}
   seal(raw);Cache recovered;Journal verify;assert(verify.load(b,recovered,Self)==-EILSEQ&&!recovered.ready());
  }PASS("valid-CRC legacy local flags, v2 reserved mute, key-bearing placeholder and excessive protected count are rejected");}
 {Radio radio;mt::Service s(radio,false,Self,81,true);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));PolicyHost h(s,radio,j,b);mt::companion::CompanionWorkspace workspace;mt::Companion c(s,h,81,workspace);
  for(bool available:{false,true}){h.serial={available,available?115200u:0u,available};admin(c,meshtastic_AdminMessage_get_device_connection_status_request_tag,0);
   const auto &p=h.output.back().packet;meshtastic_AdminMessage r{};auto stream=pb_istream_from_buffer(p.decoded.payload.bytes,p.decoded.payload.size);assert(pb_decode(&stream,meshtastic_AdminMessage_fields,&r));
   const auto &status=r.get_device_connection_status_response;assert(status.has_serial==available);if(available)assert(status.serial.is_connected&&status.serial.baud==115200);
  }assert(!b.writes&&!radio.sends);
  PASS("serial presence omitted for unsupported driver status, measured T096-style serial values retained");}
 {Radio radio;mt::Service s(radio,false,Self,81,true);Memory b;Journal j;assert(!j.load(b,s.nodes,Self));
  auto config=s.config;config.bytes[4]=1;config.seal();assert(s.apply(config)&&s.start(false,0));
  PolicyHost usb(s,radio,j,b),ble(s,radio,j,b);mt::companion::CompanionWorkspace workspace;mt::Companion c(s,usb,81,workspace),z(s,ble,81,workspace,1);usb.clients[0]=ble.clients[0]=&c;usb.clients[1]=ble.clients[1]=&z;
  handshake(c);handshake(z);auto f=frame(s,990);auto other=frame(s,990,0x42,Peer+1);
  c.receive(f.data(),f.size(),-90,2,1,mt::Companion::WorkspaceProof{true});z.receive(f.data(),f.size(),-90,2,1,mt::Companion::WorkspaceProof{true});
  c.receive(other.data(),other.size(),-90,2,1,mt::Companion::WorkspaceProof{true});z.receive(other.data(),other.size(),-90,2,1,mt::Companion::WorkspaceProof{true});
  assert(usb.previews==2&&ble.previews==2&&s.nodes.count()==2);
  const uint8_t publicBase[32]={9};assert(s.signature.trustPublicKey(Peer,publicBase,1)&&s.signature.trustPublicKey(Peer+1,publicBase,1));
  admin(c,meshtastic_AdminMessage_remove_by_nodenum_tag,Peer);assert(adminError(usb)==meshtastic_Routing_Error_NONE&&usb.forgotten==1);
  assert(s.nodes.find(Peer)<0&&s.nodes.find(Peer+1)>=0&&s.signature.keyCount(2)==1&&s.receiving()&&!s.endpointArmed());
  c.receive(f.data(),f.size(),-90,2,201,mt::Companion::WorkspaceProof{true});z.receive(f.data(),f.size(),-90,2,201,mt::Companion::WorkspaceProof{true});
  c.receive(other.data(),other.size(),-90,2,201,mt::Companion::WorkspaceProof{true});z.receive(other.data(),other.size(),-90,2,201,mt::Companion::WorkspaceProof{true});
  assert(usb.previews==3&&ble.previews==3&&s.nodes.find(Peer)>=0); // Only target's duplicate was forgotten.
  assert(s.signature.trustPublicKey(Peer,publicBase,202));b.committedError=true;
  admin(z,meshtastic_AdminMessage_remove_by_nodenum_tag,Peer);assert(adminError(ble)==meshtastic_Routing_Error_NO_INTERFACE&&!ble.forgotten);
  assert(s.signature.keyCount(203)==2&&s.nodes.find(Peer)>=0&&!s.nodes.ready()&&!s.receiving());
  PASS("verified remove forgets only target signer and both transport duplicates; same frame relearns; uncertain remove keeps authority and policy frozen");}
 {Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
  for(unsigned i=0;i<16;++i)assert(c.accept(entry(Peer+i),Self,1,0)>=0);
  assert(j.flush(b,c,0,true,true)==1);
  assert(c.accept(entry(Peer+16),Self,1,0)>=0&&c.find(Peer)<0&&c.dirty());const int before=b.writes;
  assert(j.edit(b,c,Peer,Cache::Edit::Remove,1,true)==0&&b.writes==before+1);
  Cache restored;Journal reboot;assert(!reboot.load(b,restored,Self)&&restored.find(Peer)<0);
  assert(j.edit(b,c,Peer,Cache::Edit::Remove,2,true)==0&&b.writes==before+1);
  PASS("remove of already evicted peer persists pending cache before success; clean repeated removal writes nothing");}
 printf("PASS %u node-cache groups; NodeCache=%zu Entry=%zu Journal=%zu Record=%zu Service=%zu Companion=%zu NodeInfo=%zu\n",groups,sizeof(Cache),sizeof(Cache::Entry),sizeof(Journal),Journal::RecordSize,sizeof(mt::Service),sizeof(mt::Companion),sizeof(meshtastic_NodeInfo));
 return 0;
}
