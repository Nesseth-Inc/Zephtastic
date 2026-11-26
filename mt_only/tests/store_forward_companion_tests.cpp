// SPDX-License-Identifier: GPL-3.0-only
#define CONFIG_MT_STORE_FORWARD_PSRAM 1
#include <errno.h>
#ifndef EKEYREJECTED
#define EKEYREJECTED 129 // Zephyr errno absent in Darwin libc.
#endif
#include "Companion.h"
#include "known_channel_fixture.h"
#include "modules/StoreForwardRuntime.h"
#include <cassert>
#include <iostream>
#include <vector>

namespace {
constexpr uint32_t Self=0x12345678u,Peer=0x24680135u,ArchivedId=0x55667788u;
struct Radio final:mt::Radio {
  unsigned sends=0;int sendStatus=0;bool ready()override{return true;}int configure(const mt::Config&)override{return 0;}
  int start()override{return 0;}int stop()override{return 0;}int restore()override{return 0;}
  int health()override{return 1;}bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t*,size_t)override{++sends;return sendStatus;}bool completion(int&)override{return false;}
};
struct Host final:mt::Companion::Host {
  bool accept=true,failConfigComplete=false,rejectPortOnce=false,failedHistoryCaptured=false,uptimeAvailable=true;uint32_t failPacketId=0,failPort=0,currentTime=500000,uptime=123;unsigned rejectedHistory=0,rejectedPorts=0;
  meshtastic_MeshPacket failedHistory{};
  std::vector<meshtastic_FromRadio> output;
  bool ready()const override{return true;}int save(const mt::Config&)override{return 0;}
  uint32_t wallTime()const override{return currentTime;}
  bool uptimeSeconds(uint32_t &seconds)const override{seconds=uptime;return uptimeAvailable;}
  bool send(const uint8_t *bytes,size_t size)override {
    assert(size>=4&&bytes[0]==0x94&&bytes[1]==0xc3&&size_t(bytes[2]*256+bytes[3])==size-4);
    meshtastic_FromRadio message{};auto in=pb_istream_from_buffer(bytes+4,size-4);
    assert(pb_decode(&in,meshtastic_FromRadio_fields,&message));
    if(failConfigComplete&&message.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)return false;
    if(message.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       message.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
       message.packet.decoded.portnum==failPort&&rejectPortOnce){rejectPortOnce=false;++rejectedPorts;return false;}
    if(message.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       message.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
       message.packet.id==failPacketId&&!accept){++rejectedHistory;failedHistory=message.packet;failedHistoryCaptured=true;return false;}
    output.push_back(message);return true;
  }
  bool storeAvailable=false;
  mt::modules::StoreForwardSettings storeSettings;
  unsigned storeWrites=0;
  uint32_t supportedModules()const override{
    return storeAvailable?meshtastic_ExcludedModules_STOREFORWARD_CONFIG:0;
  }
  bool moduleConfig(meshtastic_ModuleConfig &out,unsigned tag)const override{
    if(!storeAvailable||tag!=meshtastic_ModuleConfig_store_forward_tag)return false;
    out={};out.which_payload_variant=tag;
    return storeSettings.get(out.payload_variant.store_forward);
  }
  int saveModuleConfig(const meshtastic_ModuleConfig &config,uint32_t)override{
    if(!storeAvailable||config.which_payload_variant!=meshtastic_ModuleConfig_store_forward_tag)return -ENOTSUP;
    if(!storeSettings.set(config.payload_variant.store_forward))return -EINVAL;
    ++storeWrites;return 0;
  }
  void clear(){output.clear();}
  bool hasPacket(uint32_t id,unsigned port=meshtastic_PortNum_TEXT_MESSAGE_APP)const {
    for(const auto &message:output)if(message.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       message.packet.id==id&&message.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
       message.packet.decoded.portnum==port)return true;
    return false;
  }
  bool lastPacket(uint32_t id,meshtastic_MeshPacket &out,unsigned port=meshtastic_PortNum_TEXT_MESSAGE_APP)const {
    for(auto i=output.rbegin();i!=output.rend();++i)if(i->which_payload_variant==meshtastic_FromRadio_packet_tag&&
       i->packet.id==id&&i->packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
       i->packet.decoded.portnum==port){out=i->packet;return true;}
    return false;
  }
  bool lastPort(unsigned port)const {
    for(auto i=output.rbegin();i!=output.rend();++i)if(i->which_payload_variant==meshtastic_FromRadio_packet_tag&&
       i->packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&i->packet.decoded.portnum==port)return true;
    return false;
  }
};
struct Fixture {
  mt::test::KnownChannelFixtureClock clock;Radio radio;mt::Service service;Host host;
  mt::companion::CompanionWorkspace workspace;mt::Companion companion;mt::modules::StoreForwardRuntime runtime;
  mt::modules::StoreForwardSettings settings;uint32_t now=100,utc=200000;
  explicit Fixture(bool server=true):service(radio,true,Self,127,true),host(),companion(service,host,127,workspace,0){
    clock.bind(service);
    mt::Config config;config.bytes[4]=meshtastic_Config_LoRaConfig_RegionCode_US;
    auto node=config.node();node.setPublic();config.node(node);assert(service.apply(config));
    assert(service.bindStoreForwardSink(&Fixture::receiveStoreForward,this));
    assert(service.bindStoreForwardArchiveProvider(&Fixture::archive,this));
    assert(service.bindStoreForwardLocalTextSink(&Fixture::archiveLocalText,this));
    assert(service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ModuleConfig_StoreForwardConfig values=meshtastic_ModuleConfig_StoreForwardConfig_init_zero;
    values.enabled=true;values.is_server=server;values.records=16;values.heartbeat=false;
    assert(settings.set(values));runtime.poll(service,settings,true,false,true,now,7);
    assert(runtime.snapshot().enabled&&runtime.snapshot().server==server);
  }
  static void receiveStoreForward(void *p,const uint8_t *frame,size_t n,const bench::RouteMetadata &m,unsigned channel){
    auto &self=*static_cast<Fixture*>(p);self.runtime.receive(self.service,frame,n,m,uint8_t(channel),self.now,self.utc,self.now/1000);
  }
  static const mt::modules::StoreForwardArchive *archive(void *p){return static_cast<Fixture*>(p)->runtime.localArchive();}
  static bool archiveLocalText(void *p,const mt::modules::StoreForwardArchive::MessageView &message){
    auto &self=*static_cast<Fixture*>(p);return self.runtime.archiveLocalText(self.service,message);
  }
  std::vector<uint8_t> textFrame(uint32_t from,uint32_t id,const char *text="older public text"){
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;data.bitfield=1;
    data.payload.size=pb_size_t(strlen(text));memcpy(data.payload.bytes,text,data.payload.size);
    return dataFrame(from,UINT32_MAX,id,data);
  }
  std::vector<uint8_t> dataFrame(uint32_t from,uint32_t to,uint32_t id,const meshtastic_Data &data){
    uint8_t encoded[239]{};auto out=pb_ostream_from_buffer(encoded,sizeof(encoded));assert(pb_encode(&out,meshtastic_Data_fields,&data));
    mt::PrimaryChannel::Selection key{};assert(service.config.channelSelection(0,key));bench::MtCodec codec;
    assert(codec.setKey(key.hash,key.bytes,key.length));mt::PrimaryChannel::clear(key);
    uint8_t frame[255]{};size_t n=0;assert(codec.encodeZeroHopData(from,to,id,encoded,out.bytes_written,frame,sizeof(frame),n));
    frame[12]=0x91;return {frame,frame+n};
  }
  void receiveText(uint32_t id,uint32_t at=200,uint32_t epoch=UINT32_MAX){now=at;utc=epoch==UINT32_MAX?200001+at:epoch;auto frame=textFrame(Peer,id);service.rx(frame.data(),frame.size(),-70,at,99,true);}
  void input(const meshtastic_ToRadio &request,uint32_t at=300){
    uint8_t body[512]{};auto stream=pb_ostream_from_buffer(body,sizeof(body));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
    companion.byte(0x94,at,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,at,0,mt::Companion::WorkspaceProof{true});companion.byte(uint8_t(stream.bytes_written>>8),at,0,mt::Companion::WorkspaceProof{true});
    companion.byte(uint8_t(stream.bytes_written),at,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<stream.bytes_written;++i)companion.byte(body[i],at,0,mt::Companion::WorkspaceProof{true});
  }
  void handshake(bool failConfig=false){
    host.failConfigComplete=failConfig;meshtastic_ToRadio request{};
    request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=123;
    input(request);
  }
  void disconnect(){
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_disconnect_tag;
    uint8_t body[16]{};auto stream=pb_ostream_from_buffer(body,sizeof(body));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&request));
    companion.byte(0x94,400,0,mt::Companion::WorkspaceProof{true});companion.byte(0xc3,400,0,mt::Companion::WorkspaceProof{true});companion.byte(uint8_t(stream.bytes_written>>8),400,0,mt::Companion::WorkspaceProof{true});
    companion.byte(uint8_t(stream.bytes_written),400,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<stream.bytes_written;++i)companion.byte(body[i],400,0,mt::Companion::WorkspaceProof{true});
  }
  void seedStatus(){
    mt::NodeCache::Entry entry;entry.identity(Peer,127,0);
    assert(mt::NodeCache::Entry::text(entry.bytes+mt::NodeCache::Entry::Name,40,"Peer",5));
    assert(mt::NodeCache::Entry::text(entry.bytes+mt::NodeCache::Entry::Short,5,"Peer",5));
    assert(service.nodes.accept(entry,Self,-70,1)>=0);
    mt::PeerStatus status;char text[80]{};strcpy(text,"status first");assert(status.assign(text));
    assert(service.nodes.observeStatus(Peer,status));
  }
  meshtastic_Data admin(const meshtastic_AdminMessage &admin,uint32_t id,unsigned port){
    meshtastic_ToRadio req{};req.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=req.packet;p.to=Self;p.id=id;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto out=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
    assert(pb_encode(&out,meshtastic_AdminMessage_fields,&admin));p.decoded.payload.size=out.bytes_written;
    host.clear();input(req);
    for(const auto &message:host.output)if(message.which_payload_variant==meshtastic_FromRadio_packet_tag&&
       message.packet.decoded.request_id==id&&unsigned(message.packet.decoded.portnum)==port)return message.packet.decoded;
    assert(false&&"missing correlated Admin response");return {};
  }
};
void storeForwardPhoneSettings(){
  Fixture f(false);f.host.storeAvailable=true;f.handshake();
  meshtastic_AdminMessage set{},get{};
  set.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;
  set.set_module_config.which_payload_variant=meshtastic_ModuleConfig_store_forward_tag;
  auto &settings=set.set_module_config.payload_variant.store_forward;
  settings.enabled=true;settings.is_server=false;settings.records=16;settings.history_return_max=7;
  uint32_t id=700;
  auto write=[&](meshtastic_Routing_Error expected){
    auto data=f.admin(set,++id,meshtastic_PortNum_ROUTING_APP);
    meshtastic_Routing routing{};auto in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&in,meshtastic_Routing_fields,&routing)&&routing.which_variant==meshtastic_Routing_error_reason_tag&&
           routing.error_reason==expected);
  };
  write(meshtastic_Routing_Error_NONE);assert(f.host.storeWrites==1);
  get.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;
  get.get_module_config_request=meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG;
  auto read=[&](){
    auto data=f.admin(get,++id,meshtastic_PortNum_ADMIN_APP);
    meshtastic_AdminMessage response{};auto in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
    assert(pb_decode(&in,meshtastic_AdminMessage_fields,&response)&&response.which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag);
    const auto &module=response.get_module_config_response;
    assert(module.which_payload_variant==meshtastic_ModuleConfig_store_forward_tag);
    return module.payload_variant.store_forward;
  };
  settings.records=401;write(meshtastic_Routing_Error_BAD_REQUEST);
  auto current=read();assert(current.enabled&&!current.is_server&&current.records==16&&current.history_return_max==7);
  f.host.storeAvailable=false;settings.records=16;write(meshtastic_Routing_Error_BAD_REQUEST);
  assert(!read().enabled&&f.host.storeWrites==1&&f.radio.sends==0);
}
}
int main(){
  storeForwardPhoneSettings();
  for(bool server:{false,true}){
    Fixture f(server);f.handshake();f.host.clear();
    const auto frame=f.textFrame(Peer,ArchivedId);
    f.service.rx(frame.data(),frame.size(),-70,500,22,true);
    f.companion.receive(frame.data(),frame.size(),-70,3,500,f.utc,mt::Companion::WorkspaceProof{true});
    assert(f.host.hasPacket(ArchivedId)==!server);
    for(int i=0;i<3;++i)f.companion.poll(mt::Companion::WorkspaceProof{true});
    unsigned count=0;
    for(const auto &item:f.host.output)
      if(item.which_payload_variant==meshtastic_FromRadio_packet_tag&&item.packet.id==ArchivedId)++count;
    assert(count==1);
  }

  {
    Fixture f(false);f.handshake();f.host.clear();
    meshtastic_StoreAndForward message{};
    message.rr=meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY;
    message.which_variant=meshtastic_StoreAndForward_history_tag;
    message.variant.history.window=180000;
    message.variant.history.history_messages=9;
    meshtastic_Data data{};data.portnum=meshtastic_PortNum_STORE_FORWARD_APP;
    data.has_bitfield=true;data.bitfield=1;
    size_t length=0;
    assert(mt::store_forward::StoreForwardWire::encode(message,data.payload.bytes,sizeof(data.payload.bytes),&length));
    data.payload.size=pb_size_t(length);
    const auto frame=f.dataFrame(Peer,Self,ArchivedId,data);
    f.service.rx(frame.data(),frame.size(),-70,500,22,true);
    f.companion.receive(frame.data(),frame.size(),-70,3,500,f.utc,mt::Companion::WorkspaceProof{true});
    f.companion.poll(mt::Companion::WorkspaceProof{true});
    assert(f.runtime.snapshot().clientInfo.returnWindowMinutes==3);
    meshtastic_MeshPacket received{};
    assert(f.host.lastPacket(ArchivedId,received,meshtastic_PortNum_STORE_FORWARD_APP) && received.from==Peer && received.to==Self &&
           received.decoded.portnum==meshtastic_PortNum_STORE_FORWARD_APP &&
           received.decoded.payload.size==length && !memcmp(received.decoded.payload.bytes,data.payload.bytes,length));
    assert(f.radio.sends==0);
  }

  {
    Fixture f;f.handshake();meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=request.packet;packet.to=UINT32_MAX;packet.from=Peer;packet.id=ArchivedId;
    packet.channel=0;packet.hop_limit=3;packet.hop_start=1;packet.want_ack=true;
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet.decoded.payload.size=11;memcpy(packet.decoded.payload.bytes,"local hello",11);
    packet.decoded.reply_id=0x1234;packet.decoded.emoji=1;
    packet.has_rx_time=true;packet.rx_time=777777;packet.rx_snr=4.25f;packet.has_rx_rssi=true;packet.rx_rssi=-47;
    packet.via_mqtt=true;packet.transport_mechanism=meshtastic_MeshPacket_TransportMechanism_TRANSPORT_API;
    f.input(request);assert(f.runtime.localArchive()->size()==1&&f.radio.sends==0);
    const auto *record=f.runtime.localArchive()->recordAt(0);assert(record);
    assert(record->from==Self&&record->to==UINT32_MAX&&record->id==ArchivedId&&record->channel==0);
    assert(record->time==500000&&record->hasRxTime&&record->hopLimit==3&&record->hopStart==1);
    assert(record->transportMechanism==meshtastic_MeshPacket_TransportMechanism_TRANSPORT_API&&record->viaMqtt);
    assert(record->rxSnr==4.25f&&record->hasRxRssi&&record->rxRssi==-47);
    assert(record->time==500000&&record->hasRxTime); // Caller rx_time is never trusted.
    assert(record->replyId==0x1234&&record->emoji&&record->payloadSize==11&&!memcmp(record->payload,"local hello",11));
    f.radio.sendStatus=-EIO;f.service.poll(10000,17);
    assert(f.radio.sends==1&&f.runtime.localArchive()->size()==1);
  }
  {
    Fixture f;f.handshake();meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=request.packet;packet.to=Peer;packet.id=ArchivedId;
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;packet.decoded.payload.size=4;
    memcpy(packet.decoded.payload.bytes,"dm?",4);const unsigned freeBefore=f.service.endpointFree();f.input(request);
    assert(f.runtime.localArchive()->size()==0);
    assert(f.service.endpointFree()+1==freeBefore);
    request.packet.to=UINT32_MAX;request.packet.id=ArchivedId+1;request.packet.pki_encrypted=true;f.input(request);
    assert(f.runtime.localArchive()->size()==0);
    request.packet.pki_encrypted=false;request.packet.id=0;f.input(request);
    assert(f.runtime.localArchive()->size()==0);
  }
  {
    Fixture f;f.handshake();const uint8_t payload[]={'q'};uint32_t id=0;unsigned sequence=0;
    while(f.service.endpointFree()){
      assert(!f.service.submitLocalInput(meshtastic_PortNum_SERIAL_APP,payload,sizeof(payload),0,
        false,500+sequence,0x4500+sequence,id));++sequence;
    }
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=request.packet;packet.to=UINT32_MAX;packet.id=ArchivedId;packet.from=Peer;
    packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;packet.decoded.payload.size=1;
    packet.decoded.payload.bytes[0]='x';f.input(request);
    assert(f.service.endpointFree()==0&&f.runtime.localArchive()->size()==1&&f.companion.deniedTx==1);
    const auto *record=f.runtime.localArchive()->recordAt(0);
    assert(record&&record->id==ArchivedId&&record->from==Self&&record->payloadSize==1&&record->payload[0]=='x');
  }
  {
    Fixture f;f.host.currentTime=0;f.host.uptime=987;f.handshake();meshtastic_ToRadio request{};
    request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &packet=request.packet;
    packet.to=UINT32_MAX;packet.id=ArchivedId;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;packet.decoded.payload.size=1;packet.decoded.payload.bytes[0]='u';
    packet.has_rx_time=true;packet.rx_time=765432;f.input(request);
    const auto *record=f.runtime.localArchive()->recordAt(0);
    assert(record&&record->time==987&&!record->hasRxTime&&
      record->transportMechanism==meshtastic_MeshPacket_TransportMechanism_TRANSPORT_INTERNAL);
  }
  {
    Fixture f;f.handshake();meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=request.packet;packet.to=UINT32_MAX;packet.id=ArchivedId;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;packet.decoded.payload.size=1;packet.decoded.payload.bytes[0]='e';
    memset(&packet.transport_mechanism,0x7f,sizeof(packet.transport_mechanism));f.input(request);
    assert(f.runtime.localArchive()->size()==0); // Invalid enum is not narrowed or normalized.
  }
  {
    Fixture f;f.receiveText(ArchivedId,123456,0);
    const auto *record=f.runtime.localArchive()->recordAt(0);
    assert(record&&record->time==123&&!record->hasRxTime); // RF receive uses monotonic-seconds fallback.
  }
  {
    Fixture f;const uint8_t payload[]={'c','a','n','n','e','d'};uint32_t id=0;
    assert(!f.service.submitLocalInput(meshtastic_PortNum_TEXT_MESSAGE_APP,payload,sizeof(payload),0,
      false,500,0x4570,id));
    assert(id&&f.runtime.localArchive()->size()==0);
  }
  {
    Fixture f;f.receiveText(ArchivedId);assert(f.runtime.snapshot().admitted==1);
    const unsigned freeBefore=f.service.endpointFree();
    f.handshake(true);f.host.clear();f.companion.poll(mt::Companion::WorkspaceProof{true});assert(!f.host.hasPacket(ArchivedId));
    f.handshake();f.host.clear();f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.hasPacket(ArchivedId));
    meshtastic_MeshPacket packet{};assert(f.host.lastPacket(ArchivedId,packet));
    assert(packet.from==Peer&&packet.to==UINT32_MAX&&packet.id==ArchivedId&&packet.channel==0);
    assert(packet.decoded.payload.size==17&&!memcmp(packet.decoded.payload.bytes,"older public text",17));
    assert(packet.has_rx_time&&packet.rx_time==f.utc&&!packet.want_ack&&!packet.pki_encrypted&&!packet.xeddsa_signed);
    assert(packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP);
    assert(f.radio.sends==0&&f.service.endpointFree()==freeBefore);
  }
  {
    Fixture f;f.receiveText(ArchivedId);f.seedStatus();f.handshake();f.host.clear();
    f.host.failPort=meshtastic_PortNum_NODE_STATUS_APP;f.host.rejectPortOnce=true;f.companion.poll(mt::Companion::WorkspaceProof{true});
    assert(f.host.rejectedPorts==1&&!f.host.lastPort(meshtastic_PortNum_NODE_STATUS_APP)&&!f.host.hasPacket(ArchivedId));
    for(unsigned i=0;i<8 && !f.host.hasPacket(ArchivedId);++i)
      f.companion.poll(mt::Companion::WorkspaceProof{true});
    assert(f.host.lastPort(meshtastic_PortNum_NODE_STATUS_APP)&&f.host.hasPacket(ArchivedId));
    size_t statusAt=f.host.output.size(),historyAt=f.host.output.size();
    for(size_t i=0;i<f.host.output.size();++i){const auto &m=f.host.output[i];if(m.which_payload_variant==meshtastic_FromRadio_packet_tag){
      if(m.packet.decoded.portnum==meshtastic_PortNum_NODE_STATUS_APP)statusAt=i;
      if(m.packet.id==ArchivedId)historyAt=i;}}assert(statusAt<historyAt);
  }
  {
    Fixture f;f.receiveText(ArchivedId);f.handshake();f.host.clear();
    f.host.accept=false;f.host.failPacketId=ArchivedId;f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.rejectedHistory==1&&!f.host.hasPacket(ArchivedId));
    f.host.accept=true;f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.hasPacket(ArchivedId));
    meshtastic_MeshPacket packet{};assert(f.host.lastPacket(ArchivedId,packet));
    assert(f.host.failedHistoryCaptured&&packet.id==f.host.failedHistory.id&&packet.from==f.host.failedHistory.from&&
      packet.to==f.host.failedHistory.to&&packet.channel==f.host.failedHistory.channel&&
      packet.decoded.payload.size==f.host.failedHistory.decoded.payload.size&&
      !memcmp(packet.decoded.payload.bytes,f.host.failedHistory.decoded.payload.bytes,packet.decoded.payload.size));
    assert(packet.id==ArchivedId&&f.radio.sends==0);
    f.host.clear();f.disconnect();f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.output.empty());
    f.handshake();f.host.clear();f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.hasPacket(ArchivedId));
  }
  {
    Fixture f;f.receiveText(ArchivedId);f.handshake();f.host.clear();f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.hasPacket(ArchivedId));
    meshtastic_ModuleConfig_StoreForwardConfig config=meshtastic_ModuleConfig_StoreForwardConfig_init_zero;config.enabled=true;config.is_server=true;config.records=15;
    assert(f.settings.set(config));f.runtime.poll(f.service,f.settings,true,false,true,500,5);
    assert(f.runtime.localArchive()&&f.runtime.localArchive()->size()==0);
    f.host.clear();f.receiveText(ArchivedId+1,600);f.companion.poll(mt::Companion::WorkspaceProof{true});assert(f.host.hasPacket(ArchivedId+1));
    assert(!f.host.hasPacket(ArchivedId)&&f.radio.sends==0);
  }
  std::cout<<"PASS Companion local dispatch metadata/time provenance, malformed enum guard, RF uptime fallback, RX_SRC_LOCAL exclusion, queue-full ordering and history gates; no RF completion claim or archive ID lease\n";
}
