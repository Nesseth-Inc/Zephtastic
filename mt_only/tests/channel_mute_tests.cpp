// SPDX-License-Identifier: GPL-3.0-only
// M25 primary Public channel mute tests. Memory-only USB/BLE; no hardware/RF.
#include "device_config_fixture.h"

static unsigned groups=0;
#define PASS(name) do{++groups;std::cout<<"PASS " name "\n";}while(0)


constexpr uint32_t HistoryPeer=0x87654321;
struct PreservedState {unsigned keys=0;size_t history=0,known=0;};
static PreservedState seedUnrelatedState(mt::Service &service,uint32_t id=0x18180001) {
  const uint8_t publicKey[32]={9};
  assert(service.signature.trustPublicKey(HistoryPeer,publicKey,100));
  uint8_t frame[17]{};
  for(unsigned i=0;i<4;++i){frame[i]=0xff;frame[4+i]=uint8_t(HistoryPeer>>(8*i));frame[8+i]=uint8_t(id>>(8*i));}
  frame[12]=uint8_t((3u<<5)|3u);frame[13]=8;frame[15]=uint8_t(HistoryPeer);frame[16]=0x42;
  bench::RouteMetadata metadata{};metadata.channelDecoded=true;metadata.portnum=4;
  metadata.nodeInfoValid=true;metadata.encodedDataSize=1;
  assert(service.route.ingestWithMetadata(bench::Protocol::Meshtastic,frame,sizeof(frame),metadata,100,7));
  service.route.clearPending();
  PreservedState state{service.signature.keyCount(101),service.route.historyCount(),service.route.knownCount()};
  assert(state.keys==1&&state.history==1&&state.known==1);return state;
}
static void assertUnrelatedState(const mt::Service &service,const PreservedState &state) {
  assert(service.signature.keyCount(1000)==state.keys);
  assert(service.route.historyCount()==state.history&&service.route.knownCount()==state.known);
}

static meshtastic_AdminMessage channel(bool muted=false,bool fullKey=false,int index=0,
                                       meshtastic_Channel_Role role=meshtastic_Channel_Role_PRIMARY,
                                       bool module=true,bool settings=true) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
  auto &c=a.set_channel;c.index=int8_t(index);c.role=role;c.has_settings=settings;
  if(role==meshtastic_Channel_Role_PRIMARY) {
    c.settings.psk.size=fullKey?16:1;
    if(fullKey)memcpy(c.settings.psk.bytes,bench::MtNodeRecord::PublicKey,16);
    else c.settings.psk.bytes[0]=1;
  }
  c.settings.has_module_settings=module;c.settings.module_settings.is_muted=muted;return a;
}
static meshtastic_AdminMessage disabled(int index=0,bool settings=false,bool module=false) {
  return channel(false,false,index,meshtastic_Channel_Role_DISABLED,module,settings);
}
static void provision(Fixture &f,bool arm=true) {
  auto cfg=f.service.config;cfg.bytes[4]=1;cfg.setTxEnabled(arm);assert(!f.edit.save(cfg,0));
  assert(f.service.start(false,0));assert(f.service.armEndpoint(arm));
}
static const meshtastic_Channel &getChannel(Fixture &f,bool ble=false,int index=0) {
  meshtastic_AdminMessage a{};a.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;
  a.get_channel_request=index+1;auto &host=ble?f.ble:f.usb;host.out.clear();f.admin(a,ble,9);
  const auto reply=host.reply();assert(reply.which_payload_variant==meshtastic_AdminMessage_get_channel_response_tag);
  static meshtastic_Channel value;value=reply.get_channel_response;return value;
}
static const meshtastic_Channel *handshakeChannel(Fixture &f,bool ble,int index) {
  auto &host=ble?f.ble:f.usb;host.out.clear();meshtastic_ToRadio hello{};
  hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=0x25000000u+uint32_t(index);
  f.input(hello,ble,10);for(const auto &out:host.out)if(out.which_payload_variant==meshtastic_FromRadio_channel_tag&&out.channel.index==index)return &out.channel;
  return nullptr;
}
static std::vector<uint8_t> encodeAdmin(const meshtastic_AdminMessage &a) {
  uint8_t bytes[512]{};auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_AdminMessage_fields,&a));
  return {bytes,bytes+out.bytes_written};
}
static void rawAdmin(Fixture &f,const std::vector<uint8_t> &admin,bool ble,uint32_t now=1) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.to=Node;p.id=0x25010000u+now;p.want_ack=true;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
  assert(admin.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,admin.data(),admin.size());p.decoded.payload.size=admin.size();
  f.input(r,ble,now);
}
struct Snapshot {
  mt::Config config{};std::map<std::string,std::vector<uint8_t>> durable;unsigned writes=0,starts=0,stops=0,sends=0,saves=0;
  unsigned free=0;bool receiving=false,armed=false,fault=false;size_t nodes=0,history=0,known=0,routes=0,pending=0,retransmitting=0;
};
static Snapshot snapshot(Fixture &f,bool ble=false) {
  Snapshot s;s.config=f.service.config;s.durable=f.backend.entries;s.writes=f.backend.writes;s.starts=f.radio.starts;s.stops=f.radio.stops;s.sends=f.radio.sends;
  s.saves=(ble?f.ble:f.usb).saves;s.free=f.service.endpointFree();s.receiving=f.service.receiving();s.armed=f.service.endpointArmed();s.fault=f.service.faulted();
  s.nodes=f.service.nodes.count();s.history=f.service.route.historyCount();s.known=f.service.route.knownCount();s.routes=f.service.route.routeCount();
  s.pending=f.service.route.pending();s.retransmitting=f.service.route.retransmitting();return s;
}
static void unchanged(Fixture &f,const Snapshot &s,bool ble=false) {
  (void)ble;assert(f.service.config.same(s.config)&&f.backend.entries==s.durable&&f.backend.writes==s.writes&&f.radio.starts==s.starts&&
    f.radio.stops==s.stops&&f.radio.sends==s.sends&&f.service.endpointFree()==s.free&&
    f.service.receiving()==s.receiving&&f.service.endpointArmed()==s.armed&&f.service.faulted()==s.fault&&f.service.nodes.count()==s.nodes&&
    f.service.route.historyCount()==s.history&&f.service.route.knownCount()==s.known&&f.service.route.routeCount()==s.routes&&
    f.service.route.pending()==s.pending&&f.service.route.retransmitting()==s.retransmitting);
}

int main() {
  for(uint8_t model:{uint8_t(81),uint8_t(127)})for(bool ble:{false,true}) {
    Fixture f(model);provision(f);auto &host=ble?f.ble:f.usb;const auto baseWrites=f.backend.writes;
    const auto retained=seedUnrelatedState(f.service,0x25250000u+model+ble);
    f.admin(channel(true),ble,1);assert(host.error()==0&&f.service.config.channelMuted()&&f.service.receiving()&&f.service.endpointArmed());
    assertUnrelatedState(f.service,retained);
    assert(f.backend.writes==baseWrites+1);const auto &read=getChannel(f,ble);assert(read.role==meshtastic_Channel_Role_PRIMARY&&read.settings.module_settings.is_muted);
    const auto *cross=handshakeChannel(f,!ble,0);assert(cross&&cross->settings.has_module_settings&&cross->settings.module_settings.is_muted);
    mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().channelMuted());
    const auto duplicate=snapshot(f,ble);f.admin(channel(true),ble,2);assert(host.error()==0);unchanged(f,duplicate,ble);assertUnrelatedState(f.service,retained);
    f.admin(channel(false),!ble,3);auto &other=ble?f.usb:f.ble;assert(other.error()==0&&!f.service.config.channelMuted());assertUnrelatedState(f.service,retained);
    mt::SettingsJournal final;assert(!final.load(f.backend)&&!final.config().channelMuted());
  }
  PASS("both boards and USB/BLE persist true/false, cross-read through getter/handshake, restart, and elide identical saves");

  {
    Fixture f(127);provision(f);const auto writes=f.backend.writes;
    f.admin(channel(true,false));assert(f.usb.error()==0&&f.service.config.channelMuted());
    const auto afterTrue=f.backend.writes;f.admin(channel(true,true));assert(f.usb.error()==0&&f.backend.writes==afterTrue&&f.service.config.channelMuted());
    f.admin(channel(false,false,0,meshtastic_Channel_Role_PRIMARY,false));assert(f.usb.error()==0&&!f.service.config.channelMuted());
    const auto afterFalse=f.backend.writes;f.admin(channel(false));assert(f.usb.error()==0&&f.backend.writes==afterFalse);
    assert(afterTrue==writes+1&&afterFalse==writes+2);
    PASS("short/full default-Public aliases and absent/present-default module settings have exact full-replacement mute semantics");
  }
  {
    Fixture f(81);provision(f);f.admin(channel(true));assert(f.usb.error()==0&&f.service.config.channelMuted());
    f.admin(disabled());assert(f.usb.error()==0&&!f.service.config.node().isPublic()&&!f.service.config.channelMuted()&&!f.service.receiving()&&!f.service.endpointArmed());
    f.admin(channel(false));assert(f.usb.error()==0&&f.service.config.node().isPublic()&&!f.service.config.channelMuted()&&!f.service.receiving());
    f.admin(channel(true));assert(f.usb.error()==0&&f.service.config.channelMuted());
    auto bad=disabled(0,true,true);bad.set_channel.settings.module_settings.is_muted=true;const auto state=snapshot(f);
    f.admin(bad);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);unchanged(f,state);
    PASS("disabling primary resets mute and key; re-enable defaults unmuted while disabled-muted form is refused");
  }
  {
    Fixture f(127);provision(f);const auto state=snapshot(f);
    for(int index=1;index<8;++index){f.admin(disabled(index,index&1,index&1));assert(f.usb.error()==0);}
    unchanged(f,state);
    for(int index:{1,7})for(unsigned variant=0;variant<4;++variant){auto request=disabled(index,true,true);
      if(variant==0)request.set_channel.settings.uplink_enabled=true;
      if(variant==1){request.set_channel.role=meshtastic_Channel_Role_PRIMARY;request.set_channel.settings.psk.size=1;request.set_channel.settings.psk.bytes[0]=1;request.set_channel.settings.module_settings.position_precision=33;}
      if(variant==2)request.set_channel.settings.name[0]=1;
      if(variant==3)request.set_channel.settings.module_settings.position_precision=1;
      f.admin(request);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);unchanged(f,state);
    }
    for(int index:{-1,8}){f.admin(disabled(index));assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST);unchanged(f,state);}
    PASS("empty disabled writes stay inert; unsupported network/invalid promotion precision/name/position and indexes refuse");
  }
  for(bool ble:{false,true}) {
    Fixture f(ble?127:81);provision(f);const auto canonical=encodeAdmin(channel(true));
    std::vector<std::vector<uint8_t>> invalid;
    auto unknown=canonical;assert(unknown.size()>3&&unknown[2]<125);unknown[2]+=3;unknown.insert(unknown.end(),{0x98,0x06,0x00});invalid.push_back(unknown);
    auto duplicate=canonical;duplicate.insert(duplicate.end(),canonical.begin(),canonical.end());invalid.push_back(duplicate);
    auto malformed=canonical;malformed.pop_back();invalid.push_back(malformed);
    auto hostileBool=canonical;bool changed=false;for(size_t i=0;i+1<hostileBool.size();++i)if(hostileBool[i]==0x10&&hostileBool[i+1]==0x01){hostileBool[i+1]=2;changed=true;break;}assert(changed);invalid.push_back(hostileBool);
    invalid.push_back({0x8a,0x02,0x0b,0x12,0x07,0x12,0x01,0x01,0x3a,0x02,0x10,0x00,0x18,0x01});
    auto wrong=channel();wrong.set_channel.settings.module_settings.position_precision=13;invalid.push_back(encodeAdmin(wrong));
    auto key=channel();key.set_channel.settings.psk.bytes[0]=2;invalid.push_back(encodeAdmin(key));
    for(size_t i=0;i<invalid.size();++i){const auto state=snapshot(f,ble);rawAdmin(f,invalid[i],ble,uint32_t(20+i));assert((ble?f.ble:f.usb).error()==meshtastic_Routing_Error_BAD_REQUEST);unchanged(f,state,ble);}
    f.admin(channel(true),ble,50);assert((ble?f.ble:f.usb).error()==0&&f.service.config.channelMuted());
    assert(getChannel(f,ble).settings.module_settings.is_muted);
  }
  PASS("raw unknown/duplicate/truncated/noncanonical booleans plus unsupported position/key refuse on USB and BLE without poisoning a later request");

  {
    Fixture f(81);provision(f);text(f,0x25020001,10);assert(f.service.endpointFree()==3);const auto state=snapshot(f);
    f.admin(channel(true),false,11);assert(f.usb.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);unchanged(f,state);
    bool busyNotice=false;
    for(const auto &out:f.usb.out)if(out.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
      out.clientNotification.level==meshtastic_LogRecord_Level_ERROR){
      const auto *message=out.clientNotification.message;
      assert(strstr(message,"Channel setup rejected")&&strstr(message,"busy")&&!strstr(message,"mute off"));busyNotice=true;
    }
    assert(busyNotice);
    f.service.poll(300,0);assert(f.radio.sends==1&&f.service.active());const auto active=snapshot(f);
    f.admin(channel(true),false,301);assert(f.usb.error()==meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED);unchanged(f,active);
    PASS("changed mute refuses behind queued or active endpoint work without canceling jobs, arm, retries or histories");
  }
  {
    Fixture f(127);provision(f);const auto retained=seedUnrelatedState(f.service);const auto initialWrites=f.backend.writes;f.begin(100);assert(!f.service.endpointArmed());
    f.admin(channel(true),false,200);assert(f.usb.error()==0&&f.edit.open()&&f.edit.dirty()&&f.service.config.channelMuted()&&f.backend.writes==initialWrites);assertUnrelatedState(f.service,retained);
    const auto *live=handshakeChannel(f,true,0);assert(live&&live->settings.module_settings.is_muted);
    meshtastic_ToRadio disconnect{};disconnect.which_payload_variant=meshtastic_ToRadio_disconnect_tag;disconnect.disconnect=true;f.input(disconnect,false,300);
    assert(f.edit.open()&&f.edit.dirty());meshtastic_AdminMessage commit{};commit.which_payload_variant=meshtastic_AdminMessage_commit_edit_settings_tag;
    f.admin(commit,true,400);assert(f.ble.error()==0&&!f.edit.open()&&f.backend.writes==initialWrites+1&&f.service.config.channelMuted()&&f.service.endpointArmed());
    mt::SettingsJournal reboot;assert(!reboot.load(f.backend)&&reboot.config().channelMuted());assertUnrelatedState(f.service,retained);
    f.begin(500);f.admin(channel(false),false,600);assert(f.usb.error()==0&&!f.service.config.channelMuted());
    assertUnrelatedState(f.service,retained);f.admin(commit,true,700);
    assert(f.ble.error()==0&&!f.edit.open()&&f.backend.writes==initialWrites+2&&!f.service.config.channelMuted()&&f.service.endpointArmed());
    assertUnrelatedState(f.service,retained);mt::SettingsJournal unmuted;assert(!unmuted.load(f.backend)&&!unmuted.config().channelMuted());
    PASS("shared batch exposes live mute, survives one transport disconnect, commits from the other, and restores explicit TX intent");
  }
  for(bool uncertain:{false,true}) {
    Fixture f(127);provision(f);f.begin(100);f.admin(channel(true),false,200);assert(f.usb.error()==0&&f.edit.dirty());
    const auto durable=f.backend.entries;f.backend.fail=true;f.backend.uncertain=uncertain;f.commit();
    assert(f.usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f.edit.open()&&f.edit.dirty()&&f.service.config.channelMuted()&&!f.journal.ready());
    const auto writes=f.backend.writes;f.admin(channel(true),false,300);assert(f.usb.error()==meshtastic_Routing_Error_NO_INTERFACE&&f.backend.writes==writes);
    if(!uncertain)assert(f.backend.entries==durable);
  }
  PASS("failed and uncertain batch commits retain the open dirty live value, block false no-op success, and require journal recovery");

  {
    Radio radio;Backend backend;mt::SettingsJournal journal;assert(!journal.load(backend));
    mt::Service service(radio,false,Node,81,true,false);mt::SettingsEdit edit(service,journal,backend);Host host(edit,journal);mt::companion::CompanionWorkspace workspace;mt::Companion companion(service,host,81,workspace);
    auto cfg=service.config;cfg.bytes[4]=1;cfg.setTxEnabled(false);assert(!edit.save(cfg,0)&&service.start(false,0));
    meshtastic_AdminMessage request=channel(true);meshtastic_ToRadio to{};to.which_payload_variant=meshtastic_ToRadio_packet_tag;
    auto &packet=to.packet;packet.to=Node;packet.id=0x25030001;packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;packet.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
    auto encoded=pb_ostream_from_buffer(packet.decoded.payload.bytes,sizeof(packet.decoded.payload.bytes));assert(pb_encode(&encoded,meshtastic_AdminMessage_fields,&request));packet.decoded.payload.size=encoded.bytes_written;
    uint8_t raw[512];auto stream=pb_ostream_from_buffer(raw,sizeof(raw));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&to));
    for(uint8_t prefix:{uint8_t(0x94),uint8_t(0xc3),uint8_t(stream.bytes_written>>8),uint8_t(stream.bytes_written)})companion.byte(prefix,1,0,mt::Companion::WorkspaceProof{true});
    for(size_t i=0;i<stream.bytes_written;++i)companion.byte(raw[i],1,0,mt::Companion::WorkspaceProof{true});
    assert(host.error()==0&&service.config.channelMuted()&&service.receiving()&&!service.endpointArmed()&&!radio.sends);
    PASS("headless RX-only composition persists truthful channel metadata without enabling TX or display hardware");
  }
  std::cout<<"PASS channel-mute suite: "<<groups<<" semantic groups; no hardware or RF\n";
}
