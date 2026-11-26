// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "admin/PreferenceSections.h"
#include <initializer_list>

constexpr uint32_t peer=0x87654321, other=0xdab5a411;
struct Stock {
  struct {meshtastic_Config_LoRaConfig lora{};} config;
#include "oracle.inc"
};

using LoRa=meshtastic_Config_LoRaConfig;
using Decision=mt::mqtt::MqttDownlinkAdmission::Decision;

static meshtastic_AdminMessage setter(std::initializer_list<uint32_t> ids) {
  meshtastic_AdminMessage request{};
  request.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  request.set_config.which_payload_variant=meshtastic_Config_lora_tag;
  auto &l=request.set_config.payload_variant.lora;
  l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.channel_num=20;
  l.hop_limit=3;l.sx126x_rx_boosted_gain=true;l.tx_enabled=true;
  for(auto id:ids)l.ignore_incoming[l.ignore_incoming_count++]=id;
  return request;
}
static void checkList(const LoRa &l,std::initializer_list<uint32_t> ids) {
  assert(l.ignore_incoming_count==ids.size());
  unsigned i=0;for(auto id:ids)assert(l.ignore_incoming[i++]==id);
}
static LoRa read(Fixture &f,bool ble) {
  meshtastic_AdminMessage request{};
  request.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  request.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;
  f.admin(request,ble);return (ble?f.ble:f.usb).reply().get_config_response.payload_variant.lora;
}
static void reconnect(Fixture &f,bool ble,std::initializer_list<uint32_t> ids){
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_disconnect_tag;f.input(r,ble);
  auto &h=ble?f.ble:f.usb;h.out.clear();r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  r.want_config_id=901;f.input(r,ble);
  unsigned configs=0;
  for(auto &v:h.out)if(v.which_payload_variant==meshtastic_FromRadio_config_tag&&v.config.which_payload_variant==meshtastic_Config_lora_tag){
    ++configs;checkList(v.config.payload_variant.lora,ids);
  }
  assert(configs==1&&h.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag);
  assert(h.out.back().config_complete_id==901);h.out.clear();
}
static std::vector<uint8_t> frame(Fixture &f,uint32_t from,uint32_t to,uint32_t id){
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  d.payload.size=1;d.payload.bytes[0]='X';d.has_bitfield=true;
  uint8_t data[64]{},bytes[255]{};size_t n=0;
  auto enc=pb_ostream_from_buffer(data,sizeof(data));assert(pb_encode(&enc,meshtastic_Data_fields,&d));
  assert(f.service.codec.encodeZeroHopData(from,to,id,data,enc.bytes_written,bytes,sizeof(bytes),n));
  return {bytes,bytes+n};
}
static unsigned packets(const Host &h,uint32_t id){
  unsigned count=0;for(auto &v:h.out)if(v.which_payload_variant==meshtastic_FromRadio_packet_tag&&v.packet.id==id)++count;
  return count;
}
static void deliver(Fixture &f,const std::vector<uint8_t> &wire,uint32_t now) {
  f.service.rx(wire.data(),wire.size(),3,now,1);
  f.c.receive(wire.data(),wire.size(),-85,3,now,mt::Companion::WorkspaceProof{true});
  f.b.receive(wire.data(),wire.size(),-85,3,now,mt::Companion::WorkspaceProof{true});
}
static void ingress(Fixture &f) {
  const auto packet=frame(f,peer,0xffffffffu,671);
  const auto good=frame(f,other,0xffffffffu,672);
  const auto transit=frame(f,other,peer,673);
  const auto before=f.service.received;
  deliver(f,packet,25);deliver(f,frame(f,peer,Node,674),26);
  assert(f.service.received==before&&f.service.rejected>=2);
  assert(!packets(f.usb,671)&&!packets(f.ble,671)&&!packets(f.usb,674)&&!packets(f.ble,674));
  assert(!f.service.endpointControls()&&!f.radio.sends&&!f.service.route.pending());
  deliver(f,good,27);deliver(f,transit,28);
  assert(f.service.received==before+2&&packets(f.usb,672)==1&&packets(f.ble,672)==1);
  // Removing a sender does not need a new packet ID. Filtering preceded duplicate admission.
  f.admin(setter({}),false,30);assert(f.usb.error()==0);
  deliver(f,packet,31);assert(packets(f.usb,671)==1&&packets(f.ble,671)==1);
}
static void broker(Fixture &f) {
  auto cfg=f.service.config;assert(cfg.setPrimaryDownlinkEnabled(true));
  assert(f.edit.save(cfg,40)==0);
  auto r=setter({peer,Node});f.admin(r,false,41);assert(f.usb.error()==0);
  const auto wire=frame(f,peer,Node,675);
  mt::mqtt::MqttServiceEnvelopeDecoder::Decoded e{};e.kind=mt::mqtt::MqttServiceEnvelopeDecoder::PacketKind::kEncrypted;
  strcpy(e.channelId,"LongFast");e.channelIdLength=8;strcpy(e.gatewayId,"!76543210");e.gatewayIdLength=9;
  auto &p=e.packet;p.from=peer;p.to=Node;p.id=675;p.channel=wire[13];
  p.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;p.encrypted.size=wire.size()-16;
  memcpy(p.encrypted.bytes,wire.data()+16,p.encrypted.size);
  mt::mqtt::MqttDownlinkAdmission::Context c{};c.mqttEnabled=c.transportReady=c.brokerConfigured=c.encryptionEnabled=true;
  auto check=[&](){return f.service.processMqttDownlink(e,c,"!12345678",50,0);};
  assert(check()==Decision::Ignored);
  c.transportReady=false;assert(check()==Decision::TransportUnavailable);c.transportReady=true;
  p.from=Node;assert(check()==Decision::LocalSource);strcpy(e.gatewayId,"!12345678");assert(check()==Decision::GatewayEcho);
  strcpy(e.gatewayId,"!76543210");p.from=peer;p.channel=0;strcpy(e.channelId,"PKI");e.channelIdLength=3;
  p.encrypted.size=16;assert(check()==Decision::Ignored);p.from=other;assert(check()==Decision::PrivateRuntimeUnavailable);
  // A listed destination can still receive a local transmission, including when our own ID is listed.
  uint8_t payload='x',data[64]{};size_t n=0;
  assert(mt::PublicApplication::encode(8,&payload,1,0,0,data,sizeof(data),n));
  assert(!f.service.submitPublicApplication(0,peer,676,data,n,0,false,100));
  f.service.poll(10000,1);assert(f.radio.sends==1);
}
static void migration(){
  mt::Config legacy,restored;
  mt::ChannelRecord c;uint8_t key[32];memset(key,0xa5,sizeof(key));
  assert(c.set(2,"Sub",key,32)&&legacy.setSecondaryChannel(1,c));
  legacy.bytes[2]=23;legacy.seal();
  assert(mt::Config::decode(legacy.bytes,640,restored)&&restored.valid()&&!restored.ignoredSenderCount());
  mt::ChannelRecord readback;assert(restored.channelRecord(1,readback)&&!memcmp(readback.bytes,c.bytes,64));
  auto corrupt=legacy;corrupt.bytes[mt::Config::IgnoreCountOffset]=1;corrupt.seal();
  assert(!mt::Config::decode(corrupt.bytes,640,restored));
  auto previous=restored;uint32_t ids[]={peer,0,peer};assert(restored.setIgnoredSenders(ids,3)&&restored.valid());
  assert(restored.sameChannelDomain(previous)&&!restored.same(previous));
  assert(restored.channelRecord(1,readback)&&!memcmp(c.bytes,readback.bytes,64));
  assert(restored.promoteChannel(1)&&restored.ignoresIncoming(peer)&&restored.ignoresIncoming(0));
  restored.resetSecondaryChannels();restored.seal();assert(restored.valid()&&restored.ignoredSenderCount()==3);
  assert(!restored.setIgnoredSenders(ids,4));corrupt=restored;corrupt.bytes[mt::Config::IgnoreCountOffset]=1;corrupt.seal();
  assert(!mt::Config::decode(corrupt.bytes,640,previous));corrupt=restored;corrupt.bytes[mt::Config::IgnoreCountOffset]=4;corrupt.seal();
  assert(!mt::Config::decode(corrupt.bytes,640,previous));
}
static void preferenceBackup(){
  using Archive=mt::admin::PreferenceArchive;using Sections=mt::admin::PreferenceSections;
  for(unsigned version:{23u,24u}){
    Backend storage;mt::Config source;const uint32_t ids[]={peer,0};
    assert(source.setPrimaryAead(true));
    if(version==24)assert(source.setIgnoredSenders(ids,2));
    source.bytes[2]=uint8_t(version);source.seal();
    Sections::Owner host{};host.backend=&storage;host.sources.core=&source;
    host.ready=host.quiescent=[](void*){return true;};
    host.admit=[](void*,uint8_t id,const uint8_t*,size_t){return id==Sections::Core?0:-EINVAL;};
    Archive archive;Archive::SnapshotDomain domain{};domain.node=Node;domain.publicKey[0]=73;
    uint8_t section=Sections::Core;auto cb=Sections::callbacks(host);
    assert(archive.backup(storage,domain,cb,&section,1)==0);
    mt::SettingsJournal current;assert(current.load(storage)==0);
    mt::Config different;assert(different.setIgnoredSenders(ids,1)&&current.save(storage,different)==0);
    assert(archive.restore(storage,domain,cb,&section,1)==0);
    mt::SettingsJournal reboot;assert(!reboot.load(storage));
    mt::Config expected;assert(mt::Config::decode(source.bytes,source.Size,expected));
    assert(reboot.config().same(expected)&&reboot.config().primaryAead());
  }
}
int main(){
  static_assert(sizeof(LoRa::ignore_incoming)/sizeof(uint32_t)==mt::Config::IgnoreLimit);
  migration();preferenceBackup();
  for(bool ble:{false,true}){
    auto f=std::make_unique<Fixture>(ble?81:127,false);f->radio.configurable=true;f->provision();
    f->admin(setter({}),ble);assert((ble?f->ble:f->usb).error()==0);
    const auto generation=f->journal.channelGeneration();
    f->admin(setter({peer,0,peer}),ble);assert((ble?f->ble:f->usb).error()==0);
    assert(f->journal.channelGeneration()==generation&&f->service.endpointArmed()&&f->service.config.txEnabled());
    checkList(read(*f,!ble),{peer,0,peer});
    Stock stock;stock.config.lora=read(*f,ble);
    for(auto sender:{uint32_t(0),uint32_t(1),peer,other,UINT32_MAX}){
      meshtastic_MeshPacket p{};p.from=sender;p.to=peer;
      assert(f->service.config.ignoresIncoming(sender)==stock.radio(&p));
      assert(f->service.config.ignoresIncoming(sender)==stock.mqtt(p));
    }
    const auto writes=f->backend.writes;f->admin(setter({peer,0,peer}),ble);
    assert((ble?f->ble:f->usb).error()==0&&writes==f->backend.writes);
    auto restarted=std::make_unique<Fixture>(ble?81:127,false);
    restarted->backend.entries=f->backend.entries;restarted->radio.configurable=true;
    assert(!restarted->journal.load(restarted->backend)&&restarted->service.apply(restarted->journal.config()));
    assert(restarted->service.start(false,0)&&restarted->service.armEndpoint(true));
    reconnect(*restarted,false,{peer,0,peer});reconnect(*restarted,true,{peer,0,peer});
    ingress(*restarted);broker(*restarted);
    f->begin();f->admin(setter({other}),true);assert(f->ble.error()==0&&f->backend.writes==writes&&!f->service.endpointArmed()&&f->service.config.txEnabled());
    auto mixed=setter({other});mixed.set_config.payload_variant.lora.hop_limit=5;
    f->admin(mixed,true);assert(f->ble.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f->service.endpointArmed());
    mt::SettingsJournal old;assert(!old.load(f->backend)&&old.config().ignoredSenderCount()==3);
    f->commit();assert(f->usb.error()==0);mt::SettingsJournal saved;
    assert(!saved.load(f->backend)&&saved.config().ignoredSenderCount()==1&&saved.config().ignoresIncoming(other));
    f->backend.fail=true;f->admin(setter({peer}),ble);assert((ble?f->ble:f->usb).error()!=0);
    mt::SettingsJournal last;assert(!last.load(f->backend)&&last.config().same(saved.config()));
  }
  puts("PASS pinned RF/MQTT source predicates, transport gates, fresh USB/BLE sessions, Config V23 migration, journal and channel isolation");
}
