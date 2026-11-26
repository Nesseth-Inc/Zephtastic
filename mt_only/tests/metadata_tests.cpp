#include "companion/BootCounter.h"
// SPDX-License-Identifier: GPL-3.0-only
// Real Companion + pinned nanopb encoding/decoding. Synthetic identity, no RF.
#include "Companion.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#if defined(CONFIG_MT_SIGNED_WIFI_OTA)
// The signed storage fixture exercises the real boot-header reader. Inject its
// distinct result here to verify both real PhoneAPI protobuf response paths.
namespace mt {
static constexpr SignedVersionText ExpectedVersion="2.8.1+103";
const SignedVersionText &firmwareVersion() { return ExpectedVersion; }
}
#else
namespace mt { static constexpr auto &ExpectedVersion=FirmwareVersion; }
#endif
namespace {
constexpr uint32_t Node=0x12345678,Nonce=0x1234,Request=0x5678;
unsigned failures=0;
void check(bool ok,const char *what){if(!ok){++failures;std::cout<<"FAIL "<<what<<'\n';}}
struct Radio final:mt::Radio {
  unsigned starts=0,sends=0;
  bool ready()override{return true;}int start()override{++starts;return 0;}
  int stop()override{return 0;}int restore()override{return 0;}int health()override{return 1;}
  bool busy()override{return false;}uint32_t airtime(size_t)override{return 100;}
  int send(uint8_t *,size_t)override{++sends;return -EPERM;}
  bool completion(int &)override{return false;}
};
struct Host final:mt::Companion::Host {
  std::vector<meshtastic_FromRadio> packets;unsigned saves=0;bool bluetoothAvailable=true;
  bool ready()const override{return true;}
  bool bluetooth()const override{return bluetoothAvailable;}
  Security bluetoothSecurity()const override{Security link{};link.connected=true;link.level=bluetoothAvailable?2:1;return link;}
  void boardMetadata(meshtastic_DeviceMetadata &m)const override{m.canShutdown=true;}
  void boardMyInfo(meshtastic_MyNodeInfo &m,bool identify)const override{
    m.reboot_count=23;
    if(identify){memset(m.device_id.bytes,0xa5,sizeof(m.device_id.bytes));m.device_id.size=sizeof(m.device_id.bytes);}
  }
  int save(const mt::Config &)override{++saves;return -EPERM;}
  bool send(const uint8_t *p,size_t n)override {
    if(n<4||p[0]!=0x94||p[1]!=0xc3||size_t(p[2]*256+p[3])!=n-4){check(false,"invalid outgoing serial frame");return false;}
    meshtastic_FromRadio decoded{};auto stream=pb_istream_from_buffer(p+4,n-4);
    if(!pb_decode(&stream,meshtastic_FromRadio_fields,&decoded)){check(false,"outgoing FromRadio does not decode");return false;}
    packets.push_back(decoded);return true;
  }
};
// Pinned ModuleConfig getter indices and their independently specified metadata
// exclusion flags. The module field tag is getter index + 1 in this protobuf.
constexpr uint32_t ModuleBits[]={
  meshtastic_ExcludedModules_MQTT_CONFIG,meshtastic_ExcludedModules_SERIAL_CONFIG,
  meshtastic_ExcludedModules_EXTNOTIF_CONFIG,meshtastic_ExcludedModules_STOREFORWARD_CONFIG,
  meshtastic_ExcludedModules_RANGETEST_CONFIG,meshtastic_ExcludedModules_TELEMETRY_CONFIG,
  meshtastic_ExcludedModules_CANNEDMSG_CONFIG,meshtastic_ExcludedModules_AUDIO_CONFIG,
  meshtastic_ExcludedModules_REMOTEHARDWARE_CONFIG,meshtastic_ExcludedModules_NEIGHBORINFO_CONFIG,
  meshtastic_ExcludedModules_AMBIENTLIGHTING_CONFIG,meshtastic_ExcludedModules_DETECTIONSENSOR_CONFIG,
  meshtastic_ExcludedModules_PAXCOUNTER_CONFIG,meshtastic_ExcludedModules_STATUSMESSAGE_CONFIG,
  meshtastic_ExcludedModules_TRAFFICMANAGEMENT_CONFIG,meshtastic_ExcludedModules_TAK_CONFIG,
  meshtastic_ExcludedModules_MESHBEACON_CONFIG
};
std::vector<uint8_t> moduleBytes(const meshtastic_ModuleConfig &m) {
  uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  check(pb_encode(&stream,meshtastic_ModuleConfig_fields,&m),"module record cannot encode");
  return {bytes,bytes+stream.bytes_written};
}
std::vector<uint8_t> adminBytes(const meshtastic_AdminMessage &a) {
  uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
  check(pb_encode(&stream,meshtastic_AdminMessage_fields,&a),"admin record cannot encode");
  return {bytes,bytes+stream.bytes_written};
}
void nonDefaultModule(meshtastic_ModuleConfig &m) {
  auto &v=m.payload_variant;
  switch(m.which_payload_variant) {
  case meshtastic_ModuleConfig_mqtt_tag:v.mqtt.enabled=true;break;
  case meshtastic_ModuleConfig_serial_tag:v.serial.enabled=true;break;
  case meshtastic_ModuleConfig_external_notification_tag:v.external_notification.enabled=true;break;
  case meshtastic_ModuleConfig_store_forward_tag:v.store_forward.enabled=true;break;
  case meshtastic_ModuleConfig_range_test_tag:v.range_test.enabled=true;break;
  case meshtastic_ModuleConfig_telemetry_tag:v.telemetry.device_telemetry_enabled=true;break;
  case meshtastic_ModuleConfig_canned_message_tag:v.canned_message.enabled=true;break;
  case meshtastic_ModuleConfig_audio_tag:v.audio.codec2_enabled=true;break;
  case meshtastic_ModuleConfig_remote_hardware_tag:v.remote_hardware.enabled=true;break;
  case meshtastic_ModuleConfig_neighbor_info_tag:v.neighbor_info.enabled=true;break;
  case meshtastic_ModuleConfig_ambient_lighting_tag:v.ambient_lighting.led_state=true;break;
  case meshtastic_ModuleConfig_detection_sensor_tag:v.detection_sensor.enabled=true;break;
  case meshtastic_ModuleConfig_paxcounter_tag:v.paxcounter.enabled=true;break;
  case meshtastic_ModuleConfig_statusmessage_tag:strcpy(v.statusmessage.node_status,"synthetic fixture");break;
  case meshtastic_ModuleConfig_traffic_management_tag:v.traffic_management.position_min_interval_secs=1;break;
  case meshtastic_ModuleConfig_tak_tag:v.tak.team=meshtastic_Team_White;break;
  case meshtastic_ModuleConfig_mesh_beacon_tag:v.mesh_beacon.broadcast_interval_secs=3600;break;
  default:check(false,"uncovered module write fixture");
  }
}
struct Fixture {
  Radio radio;mt::Service service;Host host;mt::companion::CompanionWorkspace workspace;mt::Companion client;bool ble;uint8_t model;
  Fixture(bool b,uint8_t m,bool bluetoothAvailable=true):service(radio,
#if defined(CONFIG_MT_ALLOW_TX)
    true,
#else
    false,
#endif
    Node,m,true),client(service,host,m,workspace,b?1:0),ble(b),model(m){host.bluetoothAvailable=bluetoothAvailable;}
  void input(const meshtastic_ToRadio &message) {
    uint8_t bytes[512];auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    if(!pb_encode(&stream,meshtastic_ToRadio_fields,&message)){check(false,"synthetic request does not encode");return;}
    if(ble)client.packet(bytes,stream.bytes_written,100,mt::Companion::WorkspaceProof{true});
    else {
      client.byte(0x94,100,0,mt::Companion::WorkspaceProof{true});client.byte(0xc3,100,0,mt::Companion::WorkspaceProof{true});client.byte(stream.bytes_written>>8,100,0,mt::Companion::WorkspaceProof{true});client.byte(stream.bytes_written,100,0,mt::Companion::WorkspaceProof{true});
      for(size_t i=0;i<stream.bytes_written;++i)client.byte(bytes[i],100,0,mt::Companion::WorkspaceProof{true});
    }
  }
  void metadata(const meshtastic_DeviceMetadata &m) {
    const void *nul=memchr(m.firmware_version,0,sizeof(m.firmware_version));
    check(nul!=nullptr,"decoded version lacks bounded NUL");
    if(nul)check(std::string(m.firmware_version)==mt::ExpectedVersion,"decoded version truncated or changed");
    check(m.position_flags==service.config.gnssSettings().positionFlags&&m.canShutdown,"position/shutdown metadata");
    check(m.device_state_version==1&&m.hasBluetooth==host.bluetoothAvailable&&m.hw_model==model&&m.role==meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE,
          "adjacent metadata fields corrupted or incorrect");
    for(uint32_t bit:ModuleBits)check((m.excluded_modules&bit)!=0,"unavailable module advertised as included");
    check((m.excluded_modules&meshtastic_ExcludedModules_NETWORK_CONFIG)!=0,"unavailable network advertised as included");
    check(bool(m.excluded_modules&meshtastic_ExcludedModules_BLUETOOTH_CONFIG)==!host.bluetoothAvailable,"Bluetooth exclusion disagrees with availability");
    check(!(m.excluded_modules&~uint32_t(0x7ffff)),"unknown pinned exclusion bits advertised");
    check(!m.hasWifi&&!m.hasEthernet&&!m.hasPKC&&!m.has_xeddsa&&!m.hasRemoteHardware,"unsupported capability advertised");
  }
  void adminRequestBytes(const std::vector<uint8_t> &bytes) {
    host.packets.clear();
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=request.packet;
    p.from=Node;p.to=Node;p.id=Request;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
    check(bytes.size()<=sizeof(p.decoded.payload.bytes),"raw admin fixture too large");
    if(bytes.size()>sizeof(p.decoded.payload.bytes))return;
    memcpy(p.decoded.payload.bytes,bytes.data(),bytes.size());p.decoded.payload.size=bytes.size();input(request);
  }
  void adminRequest(const meshtastic_AdminMessage &admin) {adminRequestBytes(adminBytes(admin));}
  void expectRouting(meshtastic_Routing_Error expected,unsigned expectedNotices,const char *what) {
    unsigned routes=0,notices=0;
    for(const auto &r:host.packets) {
      if(r.which_payload_variant==meshtastic_FromRadio_clientNotification_tag) {
        ++notices;check(r.clientNotification.has_reply_id&&r.clientNotification.reply_id==Request,"module result notice lost request association");
        continue;
      }
      if(r.which_payload_variant!=meshtastic_FromRadio_packet_tag||r.packet.decoded.portnum!=meshtastic_PortNum_ROUTING_APP)continue;
      check(r.packet.decoded.request_id==Request,"module result lost routing request association");
      meshtastic_Routing result{};auto stream=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
      check(pb_decode(&stream,meshtastic_Routing_fields,&result),"module routing result decode");
      check(result.which_variant==meshtastic_Routing_error_reason_tag&&result.error_reason==expected,what);++routes;
    }
    check(routes==1&&notices==expectedNotices,"module result response count");
  }
  void getModule(unsigned index,const std::vector<uint8_t> &empty) {
    meshtastic_AdminMessage get{};get.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;
    get.get_module_config_request=static_cast<meshtastic_AdminMessage_ModuleConfigType>(index);adminRequest(get);
    unsigned replies=0;
    for(const auto &r:host.packets)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag) {
      check(r.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&r.packet.decoded.request_id==Request,"module getter failed or lost request association");
      if(r.packet.decoded.portnum!=meshtastic_PortNum_ADMIN_APP)continue;
      meshtastic_AdminMessage a{};auto stream=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
      check(pb_decode(&stream,meshtastic_AdminMessage_fields,&a),"module getter response decode");
      check(a.which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag,"module getter wrong response variant");
      if(a.which_payload_variant==meshtastic_AdminMessage_get_module_config_response_tag){++replies;check(moduleBytes(a.get_module_config_response)==empty,"excluded module getter is not disabled");}
    }
    check(replies==1,"excluded module getter response count");
  }
  void modules() {
    const auto original=service.config;
    for(unsigned index=0;index<sizeof(ModuleBits)/sizeof(ModuleBits[0]);++index) {
      meshtastic_ModuleConfig disabled{};disabled.which_payload_variant=index+1;
      const auto empty=moduleBytes(disabled);
      getModule(index,empty);
      meshtastic_AdminMessage write{};write.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;write.set_module_config=disabled;
      const auto canonical=adminBytes(write);
      adminRequestBytes(canonical);expectRouting(meshtastic_Routing_Error_NONE,0,"canonical disabled module echo was not accepted as a no-op");
      getModule(index,empty);

      nonDefaultModule(write.set_module_config);check(moduleBytes(write.set_module_config)!=empty,"non-default fixture did not change module");
      adminRequest(write);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"non-default excluded module write silently accepted");

      // Nanopb would decode these duplicate/unknown fields to the same value.
      // The wire-canonical gate must reject them rather than accepting after
      // information was dropped by the decoder.
      auto duplicate=canonical;duplicate[2]=uint8_t(duplicate[2]+empty.size());duplicate.insert(duplicate.end(),empty.begin(),empty.end());
      adminRequestBytes(duplicate);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"duplicate module oneof accepted after decoder collapse");
      auto unknown=canonical;unknown[2]=uint8_t(unknown[2]+3);unknown.insert(unknown.end(),{0x98,0x06,0x00});
      adminRequestBytes(unknown);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"unknown nested module field accepted after decoder drop");
      getModule(index,empty);
    }
    meshtastic_ModuleConfig first{};first.which_payload_variant=meshtastic_ModuleConfig_mqtt_tag;
    meshtastic_AdminMessage base{};base.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;base.set_module_config=first;
    const auto canonical=adminBytes(base);
    auto explicitZero=canonical;explicitZero[2]=uint8_t(explicitZero[2]+2);explicitZero[4]=2;explicitZero.insert(explicitZero.end(),{0x08,0x00});
    adminRequestBytes(explicitZero);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"explicit encoded default field accepted instead of canonical empty submessage");
    auto unknownAdmin=canonical;unknownAdmin.insert(unknownAdmin.end(),{0xc0,0x07,0x00});
    adminRequestBytes(unknownAdmin);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"unknown admin field accepted after decoder drop");
    auto session=canonical;session.insert(session.end(),{0xaa,0x06,0x01,0x42});
    adminRequestBytes(session);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"session passkey broadened disabled module no-op authority");
    auto noncanonical=canonical;noncanonical[1]=0x82;noncanonical.insert(noncanonical.begin()+2,0x00);
    adminRequestBytes(noncanonical);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"noncanonical protobuf varint accepted");
    auto duplicateAdmin=canonical;duplicateAdmin.insert(duplicateAdmin.end(),canonical.begin(),canonical.end());
    adminRequestBytes(duplicateAdmin);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"duplicate admin oneof accepted after decoder collapse");
    meshtastic_ModuleConfig second{};second.which_payload_variant=meshtastic_ModuleConfig_serial_tag;
    meshtastic_AdminMessage alternate{};alternate.which_payload_variant=meshtastic_AdminMessage_set_module_config_tag;alternate.set_module_config=second;
    const auto other=adminBytes(alternate);auto alternateInner=canonical;alternateInner[2]=uint8_t(alternateInner[2]+2);
    alternateInner.insert(alternateInner.end(),other.end()-2,other.end());
    adminRequestBytes(alternateInner);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"alternate nested module variant accepted after decoder collapse");
    auto alternateAdmin=canonical;alternateAdmin.insert(alternateAdmin.end(),other.begin(),other.end());
    adminRequestBytes(alternateAdmin);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"alternate duplicate admin module variant accepted");
    adminRequestBytes({0x9a,0x02,0x00});expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"missing module variant accepted");
    adminRequestBytes({0x9a,0x02,0x03,0x92,0x01,0x00});expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"unknown module variant accepted");
    auto malformed=canonical;malformed.pop_back();adminRequestBytes(malformed);expectRouting(meshtastic_Routing_Error_BAD_REQUEST,1,"malformed module request accepted");
    check(!memcmp(original.bytes,service.config.bytes,sizeof(original.bytes)),"module requests changed persisted config bytes");inert();
  }
  void inert() {
    check(client.parseErrors==0&&client.encodeErrors==0&&client.outputDrops==0,"companion parse/encode/output failure");
    check(radio.starts==0&&radio.sends==0&&host.saves==0&&!service.receiving()&&!service.endpointArmed(),"metadata request caused mutation or RF");
  }
};
uint64_t nonDiagnosticHandshakeFingerprint(const std::vector<meshtastic_FromRadio> &packets) {
  uint64_t hash=1469598103934665603ull;unsigned included=0;
  for(const auto &packet:packets) {
    if(packet.which_payload_variant==meshtastic_FromRadio_log_record_tag)continue;
    // The ordinary FromRadio stream ID advances for emitted log records; it
    // is framing metadata rather than profile content, so normalize it before
    // comparing the two diagnostic build variants.
    meshtastic_FromRadio comparable=packet;comparable.id=0;
    uint8_t bytes[4096]{};auto stream=pb_ostream_from_buffer(bytes,sizeof(bytes));
    check(pb_encode(&stream,meshtastic_FromRadio_fields,&comparable),"handshake payload cannot encode for fingerprint");
    for(size_t i=0;i<stream.bytes_written;++i){hash^=bytes[i];hash*=1099511628211ull;}
    hash^=0xff;hash*=1099511628211ull;++included;
  }
  check(included>0,"handshake fingerprint lacks stock payloads");
  return hash;
}
}
int main() {
  std::cout<<"PROFILE version="<<mt::ExpectedVersion<<" version_bytes="<<sizeof(mt::ExpectedVersion)
           <<" field_bytes="<<sizeof(meshtastic_DeviceMetadata{}.firmware_version)<<'\n';
  // Runtime assertion deliberately allows the broken source to execute both
  // paths, demonstrating that config_complete alone hides missing metadata.
  check(sizeof(mt::FirmwareVersion)<=sizeof(meshtastic_DeviceMetadata{}.firmware_version),"firmware version exceeds protobuf field including NUL");
  struct Counts {
    int length=-ENOENT,error=0;uint32_t value=0;
    int read(const char*,uint8_t *out,size_t n){if(length==4&&n==4)memcpy(out,&value,4);return length;}
    int write(const char*,const uint8_t *in,size_t n){if(error)return error;if(n==4){memcpy(&value,in,4);length=4;}return 0;}
  } counts;
  check(mt::companion::incrementBootCounter(counts)==1&&mt::companion::incrementBootCounter(counts)==2,"first and repeat boots");
  counts.error=-EIO;check(!mt::companion::incrementBootCounter(counts)&&counts.value==2,"boot count write failure");
  counts.length=3;check(!mt::companion::incrementBootCounter(counts)&&counts.value==2,"corrupt boot counter");
  // Exercise the client-visible owner and metadata records for the complete pinned catalog.
  for(unsigned model=0;model<=255;++model) {
    if(model>151 && model<255) continue;
    for(bool ble:{false,true}) {
      auto f=std::make_unique<Fixture>(ble,uint8_t(model),true);
      meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
      request.want_config_id=Nonce;f->input(request);
      bool metadata=false,owner=false,complete=false;
      for(const auto &r:f->host.packets) {
        if(r.which_payload_variant==meshtastic_FromRadio_metadata_tag)metadata=r.metadata.hw_model==model;
        if(r.which_payload_variant==meshtastic_FromRadio_node_info_tag && r.node_info.num==Node)
          owner=r.node_info.has_user && r.node_info.user.hw_model==model;
        if(r.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag)complete=r.config_complete_id==Nonce;
      }
      check(metadata && owner && complete,"catalog handshake hardware identity");
    }
  }
  const uint8_t models[] = {0, 9, 69, 81, 110, 127, 132};
  for(uint8_t model:models)for(bool ble:{false,true})for(bool bluetoothAvailable:{false,true}) {
    std::cout<<"CASE model="<<unsigned(model)<<" transport="<<(ble?"BLE":"USB")<<" bluetooth_available="<<bluetoothAvailable<<'\n';
    {
      auto f=std::make_unique<Fixture>(ble,model,bluetoothAvailable);
      check(f->service.config.setSecurityControls(true,true),"managed fixture configuration");
      meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=Nonce;f->input(request);
      bool found=false;
      for(const auto &r:f->host.packets)if(r.which_payload_variant==meshtastic_FromRadio_my_info_tag){
        found=true;check(!r.my_info.device_id.size,"managed handshake exposed hardware identifier");
      }
      check(found,"managed handshake missing public node identity");
    }
    {
      const auto before=failures;auto f=std::make_unique<Fixture>(ble,model,bluetoothAvailable);
      meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;request.want_config_id=Nonce;f->input(request);
      unsigned metadata=0,complete=0,moduleCount=0,handshakeLogs=0,localUsers=0;uint32_t moduleTags=0;bool completed=false;
      for(const auto &r:f->host.packets) {
        if(r.which_payload_variant==meshtastic_FromRadio_moduleConfig_tag) {
          const auto tag=r.moduleConfig.which_payload_variant;check(tag>=1&&tag<=17,"unexpected module in handshake");
          if(tag>=1&&tag<=17){check(!(moduleTags&(1u<<tag)),"duplicate handshake module");moduleTags|=1u<<tag;}
          meshtastic_ModuleConfig empty{};empty.which_payload_variant=tag;check(moduleBytes(r.moduleConfig)==moduleBytes(empty),"handshake excluded module not disabled");++moduleCount;
        }
        if(r.which_payload_variant==meshtastic_FromRadio_my_info_tag){
          const bool identify=!ble||bluetoothAvailable;
          const char *target = model == 127 ? "mt-zephyr-t096" : model == 81 ? "mt-zephyr-xiao" : "";
          check(!strcmp(r.my_info.pio_env,target),"MyInfo advertised an incorrect updater target");
          check(r.my_info.min_app_version==30200&&r.my_info.reboot_count==23,"MyInfo version/boot count");
          check(r.my_info.device_id.size==(identify?16:0),"silicon ID transport eligibility");
          if(identify)check(r.my_info.device_id.bytes[15]==0xa5,"silicon ID bytes");
        }
        if(r.which_payload_variant==meshtastic_FromRadio_node_info_tag && r.node_info.num==Node) {
          ++localUsers;
          check(r.node_info.has_user && r.node_info.user.hw_model==model,"local User hardware model");
        }
        if(r.which_payload_variant==meshtastic_FromRadio_metadata_tag){++metadata;check(!completed,"metadata follows config completion");f->metadata(r.metadata);}
        if(r.which_payload_variant==meshtastic_FromRadio_log_record_tag) {
          ++handshakeLogs;
          check(false,"ordinary handshake emitted a log while safe API logging is disabled");
          check(!completed,"handshake log follows config completion");
        }
        if(r.which_payload_variant==meshtastic_FromRadio_config_complete_id_tag){++complete;completed=true;check(r.config_complete_id==Nonce,"config nonce changed");}
      }
      check(moduleCount==17&&moduleTags==0x3fffe,"disabled handshake module records incomplete");
      check(localUsers==1,"handshake missing or duplicate local User");
      check(metadata==1,"handshake missing or duplicate encoded metadata");
      check(!f->host.safeApiLogEnabled(),"metadata fixture unexpectedly opted into safe API logging");
      check(handshakeLogs==0,"ordinary handshake emitted legacy or safe event logs");
      check(complete==1&&!f->host.packets.empty()&&f->host.packets.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag,"config completion missing or not final");
      std::cout<<"HANDSHAKE_NONDIAG model="<<unsigned(model)<<" transport="<<(ble?"BLE":"USB")
               <<" bluetooth_available="<<bluetoothAvailable<<" fnv64="
               <<nonDiagnosticHandshakeFingerprint(f->host.packets)<<'\n';
      f->inert();if(failures==before)std::cout<<"PASS handshake metadata roundtrip\n";
    }
    {
      const auto before=failures;auto f=std::make_unique<Fixture>(ble,model,bluetoothAvailable);
      meshtastic_AdminMessage admin{};admin.which_payload_variant=meshtastic_AdminMessage_get_device_metadata_request_tag;admin.get_device_metadata_request=true;
      meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=request.packet;
      p.from=Node;p.to=Node;p.id=Request;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
      p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;p.decoded.want_response=true;
      auto stream=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
      check(pb_encode(&stream,meshtastic_AdminMessage_fields,&admin),"admin request encoding");p.decoded.payload.size=stream.bytes_written;f->input(request);
      unsigned metadata=0,routing=0;
      for(const auto &r:f->host.packets) {
        if(r.which_payload_variant!=meshtastic_FromRadio_packet_tag)continue;
        check(r.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag,"admin response not decoded variant");
        if(r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP){++routing;continue;}
        if(r.packet.decoded.portnum!=meshtastic_PortNum_ADMIN_APP)continue;
        check(r.packet.from==Node&&r.packet.to==Node&&r.packet.decoded.request_id==Request,"admin response not associated with local request");
        meshtastic_AdminMessage decoded{};auto read=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
        if(!pb_decode(&read,meshtastic_AdminMessage_fields,&decoded)){check(false,"admin response protobuf decode");continue;}
        check(decoded.which_payload_variant==meshtastic_AdminMessage_get_device_metadata_response_tag,"wrong admin response variant");
        if(decoded.which_payload_variant==meshtastic_AdminMessage_get_device_metadata_response_tag){++metadata;f->metadata(decoded.get_device_metadata_response);}
      }
      check(metadata==1,"admin missing or duplicate encoded metadata");check(routing==0,"metadata request returned routing error instead of metadata");
      f->inert();if(failures==before)std::cout<<"PASS admin metadata roundtrip\n";
    }
    {
      const auto before=failures;auto f=std::make_unique<Fixture>(ble,model,bluetoothAvailable);f->modules();
      if(failures==before)std::cout<<"PASS exact disabled module no-ops, adversarial raw-wire refusals and getters remain inert\n";
    }
  }
  std::cout<<"RESULT failures="<<failures<<'\n';return failures?1:0;
}
