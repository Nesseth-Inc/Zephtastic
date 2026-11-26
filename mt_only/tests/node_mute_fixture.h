// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Shared M21 memory-only NodeDB/admin fixture. No device, transport or RF API.
#include "Companion.h"
#include "NodeAdmin.h"
#include "NodeJournal.h"
#include "UsbSessionEpoch.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>

constexpr uint32_t MuteLocal=0x12345678,MutePeer=0x87654321,MutePeer2=0x76543210;
struct MuteRadio:mt::Radio {
  unsigned starts=0,stops=0,sends=0;bool receiving=false;int startResult=0,stopResult=0;
  bool ready()override{return true;}int start()override{++starts;if(startResult)return startResult;receiving=true;return 0;}
  int restore()override{receiving=true;return 0;}int stop()override{++stops;if(!stopResult)receiving=false;return stopResult;}
  int health()override{return receiving?1:0;}bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return 0;}bool completion(int &)override{return false;}
};
struct MuteBackend:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> entries;unsigned writes=0;int failure=0,cut=-1;
  int read(const char *key,uint8_t *out,size_t capacity)override {
    const auto it=entries.find(key);if(it==entries.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    if(!it->second.empty())memcpy(out,it->second.data(),it->second.size());
    return int(it->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    ++writes;if(failure==1)return -EIO;
    if(cut>=0){entries[key]={p,p+size_t(cut)};return -EIO;}
    entries[key]={p,p+n};return failure==2?-EIO:0;
  }
};
struct MutePreview {uint32_t from=0,to=0,id=0;unsigned port=0;bool local=false;};
struct MuteHost:mt::Companion::Host {
  mt::Service &service;MuteRadio &radio;MuteBackend &backend;mt::NodeJournal &journal;
  std::vector<meshtastic_FromRadio> out;std::vector<std::vector<uint8_t>> records;std::vector<MutePreview> previews;
  bool dropNodeInfo=false,dropRouting=false,dropDegraded=false;unsigned dropped=0;
  bool enforceSession=false,sessionMeasured=false,sessionConnected=true,unmeasuredHandshakeBoundary=false;
  uint32_t sessionGeneration=0;unsigned handshakeBoundaries=0;
  MuteHost(mt::Service &s,MuteRadio &r,MuteBackend &b,mt::NodeJournal &j):service(s),radio(r),backend(b),journal(j){}
  bool ready()const override{return journal.ready()&&service.nodes.ready();}
  bool acceptTransportFrame(uint32_t generation)override {
    return !enforceSession||((!sessionMeasured||sessionConnected)&&generation==sessionGeneration);
  }
  bool beginUnmeasuredHandshakeBoundary()override {
    if(!unmeasuredHandshakeBoundary)return false;
    // Match the production XIAO host: the accepted canonical boundary drops
    // queued output from the abandoned logical stream before Companion resets
    // its parser and non-idempotent replay state.
    out.clear();records.clear();++handshakeBoundaries;return true;
  }
  int save(const mt::Config &)override{return -ENOTSUP;}
  int editNode(uint32_t peer,mt::NodeCache::Edit edit,uint32_t now)override {
    return mt::NodeAdmin::edit(service,radio,journal,backend,peer,edit,now);
  }
  void preview(uint32_t from,uint32_t to,uint32_t id,unsigned port,const uint8_t *,size_t,
               int16_t,int8_t,uint32_t,bool local)override{previews.push_back({from,to,id,port,local});}
  bool send(const uint8_t *p,size_t n)override {
    assert(n>=4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])==n-4);
    meshtastic_FromRadio value{};auto stream=pb_istream_from_buffer(p+4,n-4);
    assert(pb_decode(&stream,meshtastic_FromRadio_fields,&value));
    const bool routing=value.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      value.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP;
    const bool degraded=value.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
      value.clientNotification.has_reply_id&&strstr(value.clientNotification.message,"NodeDB saved");
    if((dropNodeInfo&&value.which_payload_variant==meshtastic_FromRadio_node_info_tag)||(dropRouting&&routing)||(dropDegraded&&degraded)){++dropped;return false;}
    out.push_back(value);records.emplace_back(p+4,p+n);return true;
  }
  int error(uint32_t request)const {
    for(auto it=out.rbegin();it!=out.rend();++it) {
      if(it->which_payload_variant==meshtastic_FromRadio_packet_tag&&
         it->packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&it->packet.decoded.request_id==request) {
        meshtastic_Routing route{};auto stream=pb_istream_from_buffer(it->packet.decoded.payload.bytes,it->packet.decoded.payload.size);
        assert(pb_decode(&stream,meshtastic_Routing_fields,&route));return route.error_reason;
      }
    }
    return -1;
  }
  const meshtastic_NodeInfo *node(uint32_t id)const {
    for(auto it=out.rbegin();it!=out.rend();++it)
      if(it->which_payload_variant==meshtastic_FromRadio_node_info_tag&&it->node_info.num==id)return &it->node_info;
    return nullptr;
  }
  bool notice(uint32_t request,const char *text)const {
    for(const auto &value:out)if(value.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
      value.clientNotification.has_reply_id&&value.clientNotification.reply_id==request&&strstr(value.clientNotification.message,text))return true;
    return false;
  }
};
struct MuteFixture {
  struct State {
    std::map<std::string,std::vector<uint8_t>> durable;mt::Config config{};
    mt::NodeCache::Entry peer{},peer2{};bool hasPeer2=false,privateConfigured=false,receiving=false,armed=false,nodeReady=false,journalReady=false;
    size_t count=0,endpointFree=0;unsigned writes=0,starts=0,stops=0,sends=0;
  };
  MuteRadio radio;mt::Service service;MuteBackend backend;mt::NodeJournal journal;
  MuteHost usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion companion,wireless;
  explicit MuteFixture(uint8_t model=127):service(radio,true,MuteLocal,model,true,false),usb(service,radio,backend,journal),ble(service,radio,backend,journal),
    companion(service,usb,model,workspace,0),wireless(service,ble,model,workspace,1) {
    mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);config.seal();
    assert(service.apply(config)&&!journal.load(backend,service.nodes,MuteLocal));
  }
  static std::vector<uint8_t> inputWire(const meshtastic_ToRadio &value) {
    uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&stream,meshtastic_ToRadio_fields,&value));return {bytes,bytes+stream.bytes_written};
  }
  static std::vector<uint8_t> framed(const std::vector<uint8_t> &payload) {
    assert(!payload.empty()&&payload.size()<=512);
    std::vector<uint8_t> bytes{0x94,0xc3,uint8_t(payload.size()>>8),uint8_t(payload.size())};
    bytes.insert(bytes.end(),payload.begin(),payload.end());return bytes;
  }
  static std::vector<uint8_t> adminWire(const meshtastic_AdminMessage &admin) {
    uint8_t bytes[512]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&stream,meshtastic_AdminMessage_fields,&admin));return {bytes,bytes+stream.bytes_written};
  }
  static std::vector<uint8_t> toggleWire(uint32_t target) {
    meshtastic_AdminMessage admin{};admin.which_payload_variant=meshtastic_AdminMessage_toggle_muted_node_tag;
    admin.toggle_muted_node=target;return adminWire(admin);
  }
  void input(const meshtastic_ToRadio &value,bool useBle,uint32_t now) {
    auto bytes=inputWire(value);
    if(useBle)wireless.packet(bytes.data(),bytes.size(),now,mt::Companion::WorkspaceProof{true});
    else for(uint8_t byte:framed(bytes))companion.byte(byte,now,0,mt::Companion::WorkspaceProof{true});
  }
  void rawAdmin(const std::vector<uint8_t> &wire,uint32_t request,bool useBle=false,uint32_t destination=MuteLocal,uint32_t now=1) {
    meshtastic_ToRadio value{};value.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=value.packet;
    p.to=destination;p.id=request;p.want_ack=true;p.pki_encrypted=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    assert(wire.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,wire.data(),wire.size());p.decoded.payload.size=wire.size();input(value,useBle,now);
  }
  void toggle(uint32_t target,uint32_t request,bool useBle=false,uint32_t now=1){rawAdmin(toggleWire(target),request,useBle,MuteLocal,now);}
  void handshake(bool useBle=false,uint32_t nonce=1) {
    meshtastic_ToRadio value{};value.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;value.want_config_id=nonce;input(value,useBle,nonce);
  }
  void seed(uint32_t id=MutePeer,const char *name="Synthetic peer") {
    mt::NodeCache::Entry entry{};entry.identity(id,81,0);
    assert(entry.text(entry.bytes+entry.Name,40,name,strlen(name)+1)&&entry.text(entry.bytes+entry.Short,5,"SYN",4));
    assert(service.nodes.accept(entry,MuteLocal,3,0)>=0);
  }
  State snapshot() {
    State value;value.durable=backend.entries;value.config=service.config;value.count=service.nodes.count();value.writes=backend.writes;
    value.privateConfigured=service.privateConfigured();value.receiving=service.receiving();value.armed=service.endpointArmed();
    value.endpointFree=service.endpointFree();value.nodeReady=service.nodes.ready();value.journalReady=journal.ready();
    value.starts=radio.starts;value.stops=radio.stops;value.sends=radio.sends;
    const int peer=service.nodes.find(MutePeer);assert(peer>=0);value.peer=*service.nodes.get(unsigned(peer));
    const int peer2=service.nodes.find(MutePeer2);value.hasPeer2=peer2>=0;if(value.hasPeer2)value.peer2=*service.nodes.get(unsigned(peer2));
    return value;
  }
  void assertUnchanged(const State &value) {
    assert(backend.entries==value.durable&&backend.writes==value.writes&&service.config.same(value.config)&&service.nodes.count()==value.count&&
      service.privateConfigured()==value.privateConfigured&&service.receiving()==value.receiving&&service.endpointArmed()==value.armed&&
      service.endpointFree()==value.endpointFree&&service.nodes.ready()==value.nodeReady&&journal.ready()==value.journalReady&&
      radio.starts==value.starts&&radio.stops==value.stops&&radio.sends==value.sends);
    const int peer=service.nodes.find(MutePeer);assert(peer>=0&&!memcmp(&value.peer,service.nodes.get(unsigned(peer)),sizeof(value.peer)));
    const int peer2=service.nodes.find(MutePeer2);assert((peer2>=0)==value.hasPeer2);
    if(value.hasPeer2)assert(!memcmp(&value.peer2,service.nodes.get(unsigned(peer2)),sizeof(value.peer2)));
  }
  std::vector<uint8_t> textFrame(uint32_t from,uint32_t to,uint32_t id,const char *text) {
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
    data.payload.size=strlen(text);memcpy(data.payload.bytes,text,data.payload.size);
    uint8_t encoded[239]{};auto stream=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&stream,meshtastic_Data_fields,&data));
    std::vector<uint8_t> wire(255);size_t size=0;assert(service.codec.encodeZeroHopData(from,to,id,encoded,stream.bytes_written,wire.data(),wire.size(),size));
    wire.resize(size);wire[12]=0;return wire;
  }
  void receiveBoth(const std::vector<uint8_t> &wire,uint32_t now) {
    companion.receive(wire.data(),wire.size(),-80,3,now,mt::Companion::WorkspaceProof{true});wireless.receive(wire.data(),wire.size(),-80,3,now,mt::Companion::WorkspaceProof{true});
  }
};
