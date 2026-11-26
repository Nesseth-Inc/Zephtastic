// SPDX-License-Identifier: GPL-3.0-only
// Independent client traces against actual Companion/Service/NodeJournal.
// Expectations follow pinned stock AdminModule 508-579, Router 1710-1726 and
// NodeDB protected-node policy, with explicit ZT absent-peer/error differences.
// Synthetic peers and memory-only failed writes; never hardware or credentials.
#include "Companion.h"
#include "NodeJournal.h"
#include "NodeAdmin.h"
#include "monocypher.h"
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
constexpr uint32_t LOCAL=0x12345678,PEER=0x87654321;
struct Radio:mt::Radio {
  unsigned sends=0;bool receiving=false;int stopResult=0;
  bool ready()override{return true;}int start()override{receiving=true;return 0;}
  int stop()override{if(!stopResult)receiving=false;return stopResult;}int restore()override{return 0;}int health()override{return receiving?1:0;}
  bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return 0;}bool completion(int &)override{return false;}
};
struct Backend:mt::SettingsBackend {
  std::map<std::string,std::vector<uint8_t>> bytes;unsigned writes=0;int failure=0;
  int read(const char *key,uint8_t *out,size_t cap)override {
    auto it=bytes.find(key);if(it==bytes.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *p,size_t n)override {
    ++writes;if(failure==1)return -EIO;bytes[key]={p,p+n};return failure==2?-EIO:0;
  }
};
struct Host:mt::Companion::Host {
  mt::Service &service;Radio &radio;Backend &backend;mt::NodeJournal &journal;
  std::vector<meshtastic_FromRadio> out;unsigned previews=0;Serial serial{};
  mt::Companion *clients[2]{};
  Host(mt::Service &s,Radio &r,Backend &b,mt::NodeJournal &j):service(s),radio(r),backend(b),journal(j){}
  bool ready()const override{return true;}
  Serial serialStatus()const override{return serial;}
  int save(const mt::Config &)override{return -ENOTSUP;}
  static void removed(uint32_t peer,void *context) {
    auto &host=*static_cast<Host *>(context);for(auto *client:host.clients)if(client)client->forgetPeer(peer);
  }
  int editNode(uint32_t peer,mt::NodeCache::Edit edit,uint32_t now)override {
    return mt::NodeAdmin::edit(service,radio,journal,backend,peer,edit,now,false,removed,this);
  }
  void preview(uint32_t,uint32_t,uint32_t,unsigned,const uint8_t *,size_t,int16_t,int8_t,uint32_t,bool)override{++previews;}
  bool send(const uint8_t *p,size_t n)override {
    assert(n>=4&&p[0]==0x94&&p[1]==0xc3&&size_t(p[2]*256+p[3])==n-4);
    meshtastic_FromRadio msg{};auto b=pb_istream_from_buffer(p+4,n-4);
    assert(pb_decode(&b,meshtastic_FromRadio_fields,&msg));out.push_back(msg);return true;
  }
  int error(uint32_t id)const {
    for(const auto &r:out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.request_id==id&&r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP) {
      meshtastic_Routing v{};auto b=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
      assert(pb_decode(&b,meshtastic_Routing_fields,&v));return v.which_variant==meshtastic_Routing_error_reason_tag?int(v.error_reason):-2;
    }
    return -1;
  }
};
struct Fixture {
  Radio radio;mt::Service service;Backend backend;mt::NodeJournal journal;Host usb,ble;mt::companion::CompanionWorkspace workspace;mt::Companion u,b;uint32_t id=100;
  Fixture():service(radio,true,LOCAL,127,true,false),usb(service,radio,backend,journal),ble(service,radio,backend,journal),u(service,usb,127,workspace,0),b(service,ble,127,workspace,1) {
    mt::Config config;config.bytes[4]=1;auto record=config.node();record.setPublic();config.node(record);
    assert(service.apply(config)&&!journal.load(backend,service.nodes,LOCAL));
    usb.clients[0]=ble.clients[0]=&u;usb.clients[1]=ble.clients[1]=&b;
    snapshot(false);snapshot(true);
  }
  void input(const meshtastic_ToRadio &r,bool wireless=false,uint32_t now=100) {
    uint8_t bytes[512];auto s=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&s,meshtastic_ToRadio_fields,&r));
    if(wireless)b.packet(bytes,s.bytes_written,now,mt::Companion::WorkspaceProof{true});
    else {u.byte(0x94,now,0,mt::Companion::WorkspaceProof{true});u.byte(0xc3,now,0,mt::Companion::WorkspaceProof{true});u.byte(s.bytes_written>>8,now,0,mt::Companion::WorkspaceProof{true});u.byte(s.bytes_written,now,0,mt::Companion::WorkspaceProof{true});for(size_t i=0;i<s.bytes_written;++i)u.byte(bytes[i],now,0,mt::Companion::WorkspaceProof{true});}
  }
  uint32_t admin(unsigned tag,uint32_t target,bool wireless=false,uint32_t destination=LOCAL) {
    meshtastic_AdminMessage a{};a.which_payload_variant=tag;
    switch(tag) {
      case 38:a.remove_by_nodenum=target;break;case 39:a.set_favorite_node=target;break;
      case 40:a.remove_favorite_node=target;break;case 47:a.set_ignored_node=target;break;
      case 48:a.remove_ignored_node=target;break;default:assert(false);
    }
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=++id;p.to=destination;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
    assert(pb_encode(&s,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=s.bytes_written;
    input(r,wireless,id*10);return id;
  }
  std::map<uint32_t,meshtastic_NodeInfo> snapshot(bool wireless=false) {
    Host &h=wireless?ble:usb;h.out.clear();meshtastic_ToRadio r{};
    r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=++id;input(r,wireless,id*10);
    std::map<uint32_t,meshtastic_NodeInfo> peers;
    for(const auto &item:h.out)if(item.which_payload_variant==meshtastic_FromRadio_node_info_tag)peers[item.node_info.num]=item.node_info;
    return peers;
  }
  void seed(uint32_t peer=PEER,const char *name="Synthetic peer") {
    mt::NodeCache::Entry e;e.identity(peer,81,0);
    assert(e.text(e.bytes+e.Name,40,name,strlen(name)+1)&&e.text(e.bytes+e.Short,5,"SYN",4));
    assert(service.nodes.accept(e,LOCAL,3,0)>=0);
  }
  std::vector<uint8_t> frame(uint32_t peer,uint32_t packet,bool nodeInfo=false) {
    meshtastic_Data data{};data.portnum=nodeInfo?meshtastic_PortNum_NODEINFO_APP:meshtastic_PortNum_TEXT_MESSAGE_APP;data.has_bitfield=true;
    if(nodeInfo) {
      meshtastic_User user{};strcpy(user.long_name,"Updated remote name");strcpy(user.short_name,"UPD");
      user.hw_model=meshtastic_HardwareModel_SEEED_XIAO_S3;
      auto s=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));assert(pb_encode(&s,meshtastic_User_fields,&user));data.payload.size=s.bytes_written;
    }else {data.payload.size=4;memcpy(data.payload.bytes,"test",4);}
    uint8_t bytes[239],wire[255];size_t length=0;auto s=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&s,meshtastic_Data_fields,&data)&&service.codec.encodeZeroHopData(peer,LOCAL,packet,bytes,s.bytes_written,wire,sizeof(wire),length));
    wire[12]=8;return {wire,wire+length};
  }
  void receive(const std::vector<uint8_t> &wire,uint32_t now=5000) {
    service.rx(wire.data(),wire.size(),3,now,700);u.receive(wire.data(),wire.size(),-90,3,now,mt::Companion::WorkspaceProof{true});b.receive(wire.data(),wire.size(),-90,3,now,mt::Companion::WorkspaceProof{true});
  }
};
static unsigned failed=0;
static void check(bool ok,const char *name){failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";}
static void flagsAndReadback() {
  auto f=std::make_unique<Fixture>();f->seed();auto request=f->admin(39,PEER);bool ok=f->usb.error(request)==0;
  auto view=f->snapshot(true);ok&=view.count(PEER)&&view.at(PEER).is_favorite&&!view.at(PEER).is_ignored;
  assert(f->service.start(false,1000));auto wire=f->frame(PEER,1234,true);f->receive(wire);f->service.stop();
  view=f->snapshot();ok&=view.count(PEER)&&view.at(PEER).is_favorite&&!strcmp(view.at(PEER).user.long_name,"Updated remote name");
  request=f->admin(40,PEER,true);ok&=f->ble.error(request)==0;view=f->snapshot();ok&=!view.at(PEER).is_favorite;
  check(ok,"wire_favorite_cross_transport_and_received_identity_preserve_local_flags");
}
static void ignoreAbsentAndReceive() {
  auto f=std::make_unique<Fixture>();auto request=f->admin(47,PEER);bool ok=f->usb.error(request)==0;
  auto view=f->snapshot();ok&=view.count(PEER)&&view.at(PEER).is_ignored&&!view.at(PEER).has_user;
  assert(f->service.start(false,1000)&&f->service.armEndpoint(true));auto wire=f->frame(PEER,2345);
  const auto free=f->service.endpointFree();f->usb.out.clear();f->ble.out.clear();f->receive(wire);
  ok&=f->service.endpointFree()==free&&!f->u.receivedMessages&&!f->b.receivedMessages&&!f->usb.previews&&!f->ble.previews&&f->usb.out.empty()&&f->ble.out.empty();
  f->service.stop();request=f->admin(48,PEER,true);ok&=f->ble.error(request)==0;
  assert(f->service.start(false,6000));f->receive(wire,7000);
  ok&=f->u.receivedMessages==1&&f->b.receivedMessages==1&&f->usb.previews==1&&f->ble.previews==1&&!f->radio.sends;
  check(ok,"ignore_absent_peer_blocks_ack_and_both_clients_without_poisoning_duplicate_history");
}
static void errorsAndRemoval() {
  auto f=std::make_unique<Fixture>();bool ok=true;
  // M26 aligns only missing favorite/unfavorite with stock no-op success.
  // Missing unignore still rejects; no operation below may allocate or write.
  const auto writes=f->backend.writes;const auto durable=f->backend.bytes;
  const auto generation=f->journal.generation();
  for(bool ble:{false,true})for(unsigned tag:{39u,40u,48u}) {
    const auto id=f->admin(tag,PEER,ble);
    ok&=(ble?f->ble:f->usb).error(id)==(tag==48u?32:0)&&!f->service.nodes.count()&&
      f->backend.writes==writes&&f->backend.bytes==durable&&
      f->journal.generation()==generation&&!f->radio.sends;
  }
  for(unsigned tag:{38u,39u,40u,47u,48u})for(uint32_t target:{0u,1u,LOCAL,0xfffffffeu,0xffffffffu}) {
    auto id=f->admin(tag,target);ok&=f->usb.error(id)==32&&!f->service.nodes.count();
  }
  auto id=f->admin(47,PEER,false,PEER);ok&=f->usb.error(id)!=0&&!f->service.nodes.count();
  id=f->admin(38,PEER);ok&=f->usb.error(id)==0;
  f->seed();id=f->admin(47,PEER);ok&=f->usb.error(id)==0;id=f->admin(38,PEER,true);
  ok&=f->ble.error(id)==0&&!f->service.nodes.count()&&!f->radio.sends;
  mt::NodeJournal reboot;mt::NodeCache restored;ok&=!reboot.load(f->backend,restored,LOCAL)&&!restored.count();
  check(ok,"invalid_self_remote_and_absent_operation_contracts_and_durable_full_removal");
}
static void protectedChurn() {
  auto f=std::make_unique<Fixture>();bool ok=true;
  for(uint32_t i=0;i<14;++i) {auto id=f->admin(47,PEER+i);ok&=f->usb.error(id)==0;}
  const auto writes=f->backend.writes;auto request=f->admin(47,PEER+14);
  ok&=f->usb.error(request)!=0&&f->backend.writes==writes;
  for(uint32_t i=100;i<180;++i)f->seed(PEER+i);
  const auto view=f->snapshot(true);
  for(uint32_t i=0;i<14;++i)ok&=view.count(PEER+i)&&view.at(PEER+i).is_ignored;
  request=f->admin(39,PEER);ok&=f->usb.error(request)==0;
  const auto final=f->snapshot();ok&=final.at(PEER).is_favorite&&final.at(PEER).is_ignored&&f->service.nodes.count()==16;
  check(ok,"fourteen_union_protected_peers_survive_evictable_churn_without_cap_overflow");
}
static void uncertainPersistence() {
  bool ok=true;
  for(int mode:{1,2}) {
    auto f=std::make_unique<Fixture>();f->seed();auto id=f->admin(39,PEER);ok&=f->usb.error(id)==0;
    f->backend.failure=mode;id=f->admin(47,PEER);ok&=f->usb.error(id)!=0;
    const auto view=f->snapshot();ok&=view.at(PEER).is_favorite&&!view.at(PEER).is_ignored&&!f->service.nodes.ready();
    const auto writes=f->backend.writes;f->admin(48,PEER);ok&=f->backend.writes==writes;
    f->backend.failure=0;mt::NodeJournal reboot;mt::NodeCache restored;ok&=!reboot.load(f->backend,restored,LOCAL);
    // Restore through a fresh companion: inspect public NodeInfo, not flag layout.
    f->service.nodes=restored;const auto after=f->snapshot(true);ok&=after.at(PEER).is_favorite&&after.at(PEER).is_ignored==(mode==2)&&!f->radio.sends;
  }
  check(ok,"failed_before_or_after_write_never_acknowledges_or_publishes_unverified_edit");
}
static std::vector<uint8_t> legacyRecord() {
  // Recorded M8 schema: 16-byte header + sixteen 92-byte peers + CRC32.
  std::vector<uint8_t> raw(1492);memcpy(raw.data(),"ZNJ\1",4);raw[4]=1;raw[8]=1;raw[10]=92;
  for(unsigned i=0;i<4;++i)raw[16+i]=uint8_t(PEER>>(i*8));
  raw[20]=81;
  memcpy(raw.data()+28,"Legacy peer",12);memcpy(raw.data()+68,"OLD",4);
  // The old remote-owned User.is_unmessagable bits are unrelated to local ignore.
  raw[106]=3;uint32_t crc=0xffffffffu;
  for(size_t i=0;i<raw.size()-4;++i){crc^=raw[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);}
  crc=~crc;for(unsigned i=0;i<4;++i)raw[raw.size()-4+i]=uint8_t(crc>>(i*8));return raw;
}
static void migrationAndTornRecord() {
  auto f=std::make_unique<Fixture>();f->backend.bytes["mtbase/nodes0"]=legacyRecord();
  bool ok=!f->journal.load(f->backend,f->service.nodes,LOCAL)&&!f->backend.writes;
  auto view=f->snapshot();ok&=view.at(PEER).user.has_is_unmessagable&&view.at(PEER).user.is_unmessagable&&!view.at(PEER).is_favorite&&!view.at(PEER).is_ignored;
  const auto request=f->admin(39,PEER);ok&=f->usb.error(request)==0&&f->backend.writes==1&&f->backend.bytes.at("mtbase/nodes0")==legacyRecord();
  mt::NodeCache restored;mt::NodeJournal reboot;ok&=!reboot.load(f->backend,restored,LOCAL);
  f->service.nodes=restored;view=f->snapshot(true);ok&=view.at(PEER).is_favorite&&!view.at(PEER).is_ignored;
  check(ok,"legacy_v1_load_is_read_only_and_remote_flags_never_become_local_trust_or_ignore");
  // A torn candidate beside a valid legacy record must not silently roll back
  // an apparently successful block operation or enable further persistence.
  f->backend.bytes["mtbase/nodes1"].resize(100);mt::NodeJournal broken;mt::NodeCache recovered;
  const auto rc=broken.load(f->backend,recovered,LOCAL);const auto writes=f->backend.writes;
  ok=rc<0&&!broken.ready()&&!recovered.ready()&&recovered.count()==1&&recovered.get(0)->id()==PEER;
  ok&=broken.flush(f->backend,recovered,90000,true,true)<0&&f->backend.writes==writes;
  check(ok,"torn_v2_beside_valid_v1_recovers_peer_read_only_without_overwrite");
}
static void serialUnknownAndNoPrivateCapability() {
  auto f=std::make_unique<Fixture>();bool ok=true;
  for(bool available:{false,true})for(bool connected:{false,true}) {
    f->usb.serial={connected,connected?115200u:0u,available};f->usb.out.clear();
    meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_device_connection_status_request_tag;a.get_device_connection_status_request=true;
    meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &p=r.packet;p.id=++f->id;p.to=LOCAL;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    auto s=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));assert(pb_encode(&s,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=s.bytes_written;f->input(r);
    bool found=false;
    for(const auto &record:f->usb.out)if(record.which_payload_variant==meshtastic_FromRadio_packet_tag&&record.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&record.packet.decoded.request_id==p.id) {
      meshtastic_AdminMessage response{};auto in=pb_istream_from_buffer(record.packet.decoded.payload.bytes,record.packet.decoded.payload.size);assert(pb_decode(&in,meshtastic_AdminMessage_fields,&response));
      const auto &status=response.get_device_connection_status_response;
      found=response.which_payload_variant==meshtastic_AdminMessage_get_device_connection_status_response_tag;
      ok&=status.has_serial==available&&!status.has_wifi&&!status.has_ethernet&&status.serial.is_connected==(available&&connected)&&status.serial.baud==(available?f->usb.serial.baud:0);
    }
    ok&=found;
  }
  f->snapshot();bool metadata=false,security=false;
  for(const auto &record:f->usb.out) {
    if(record.which_payload_variant==meshtastic_FromRadio_metadata_tag){metadata=true;ok&=!record.metadata.hasPKC;}
    if(record.which_payload_variant==meshtastic_FromRadio_config_tag&&record.config.which_payload_variant==meshtastic_Config_security_tag) {
      security=true;const auto &value=record.config.payload_variant.security;ok&=!value.private_key.size&&!value.public_key.size&&!value.admin_key_count;
    }
  }
  check(ok&&metadata&&security,"serial_unavailable_is_omitted_and_no_private_runtime_capability_is_advertised");
}
static void realAdminRadioGate() {
  bool ok=true;
  for(int fault:{0,1,2}) {
    auto f=std::make_unique<Fixture>();f->seed();assert(f->service.start(false,100));
    if(fault==1)f->radio.stopResult=-EIO;
    if(fault==2)f->backend.failure=1;
    const auto initialNodes=f->service.nodes.count();const auto initialKeys=f->service.signature.keyCount(100);
    const auto initialHistory=f->service.route.historyCount();
    const auto request=f->admin(39,PEER);ok&=(f->usb.error(request)==0)==(fault==0);
    ok&=!f->radio.sends&&f->service.nodes.count()==initialNodes&&
        f->service.signature.keyCount(100)==initialKeys&&f->service.route.historyCount()==initialHistory;
    if(fault==0)ok&=f->service.receiving()&&!f->service.endpointArmed()&&!f->service.faulted();
    if(fault==1)ok&=f->service.receiving()&&!f->service.endpointArmed()&&f->service.faulted()&&
        f->radio.receiving&&!f->backend.writes;
    if(fault==2)ok&=!f->service.receiving()&&!f->service.endpointArmed()&&!f->radio.receiving&&
        !f->service.nodes.ready()&&!f->service.start(false,300);
  }
  check(ok,"real_node_admin_restores_only_verified_rx_and_never_writes_after_failed_stop");
}
static void targetedTransientRemoval() {
  bool ok=true;uint8_t scalar[32]{},publicKey[32];scalar[0]=8;scalar[31]=64;crypto_x25519_public_key(publicKey,scalar);
  for(int fault:{0,1,2}) {
    auto f=std::make_unique<Fixture>();f->seed();f->seed(PEER+1);
    auto request=f->admin(39,PEER);ok&=f->usb.error(request)==0;
    ok&=f->service.signature.trustPublicKey(PEER,publicKey,100)&&f->service.signature.trustPublicKey(PEER+1,publicKey,100);
    assert(f->service.start(false,1000));auto first=f->frame(PEER,5678),other=f->frame(PEER+1,6789);f->receive(first);f->receive(other);
    ok&=f->u.receivedMessages==2&&f->b.receivedMessages==2;
    f->service.stop();f->backend.failure=fault;request=f->admin(38,PEER);
    ok&=(f->usb.error(request)==0)==(fault==0)&&f->service.signature.keyCount(6000)==(fault?2u:1u);
    if(!fault) {
      assert(f->service.start(false,6000));f->receive(first,6500);f->receive(other,6600);
      ok&=f->u.receivedMessages==3&&f->b.receivedMessages==3;
    }
  }
  crypto_wipe(scalar,32);
  check(ok,"verified_remove_clears_only_target_signer_and_both_receive_duplicates_failures_preserve_them");
}
static void evictedRemovalMustStillPersistAbsence() {
  auto f=std::make_unique<Fixture>();f->seed();assert(f->journal.flush(f->backend,f->service.nodes,100,true,true)==1);
  for(uint32_t i=1;i<=20;++i)f->seed(PEER+i);
  bool ok=f->service.nodes.find(PEER)<0&&f->service.nodes.dirty();const auto writes=f->backend.writes;
  auto request=f->admin(38,PEER);ok&=f->usb.error(request)==0&&f->backend.writes==writes+1;
  mt::NodeCache restored;mt::NodeJournal restarted;ok&=!restarted.load(f->backend,restored,LOCAL)&&restored.find(PEER)<0;
  request=f->admin(38,PEER);ok&=f->usb.error(request)==0&&f->backend.writes==writes+1;
  check(ok,"removing_ram_evicted_peer_commits_absence_so_restart_cannot_resurrect_it");
}
int main(){flagsAndReadback();ignoreAbsentAndReceive();errorsAndRemoval();protectedChurn();uncertainPersistence();migrationAndTornRecord();serialUnknownAndNoPrivateCapability();realAdminRadioGate();targetedTransientRemoval();evictedRemovalMustStillPersistAbsence();return failed?1:0;}
