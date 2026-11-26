// SPDX-License-Identifier: GPL-3.0-only
// Direct synthetic tests for Companion's trusted snapshot callback. These
// fixtures do not establish authentication, RF delivery, or encrypted wire use.
#include "pki_runtime_fixture.h"
#include "admin/RemoteAdminProtocol.h"

namespace {
using namespace mt::pki;
using mt::admin::remote_protocol::MaxDataBytes;
using mt::admin::remote_protocol::MaxPayloadBytes;
using mt::admin::remote_protocol::Operation;
using mt::admin::remote_protocol::Query;

struct AdminHost final : mt::Companion::Host {
  meshtastic_Config_SecurityConfig security{};
  meshtastic_Config_NetworkConfig network{};
  unsigned adminKeyReads=0,passkeyCalls=0,saveAdminKeyCalls=0;
  unsigned networkReads=0,moduleReads=0,sendCalls=0,saveCalls=0;
  unsigned nodeEditCalls=0,localNodeEditCalls=0,remoteFavoriteCalls=0;
  uint32_t lastEditedNode=0;
  mt::NodeCache::Edit lastNodeEdit=mt::NodeCache::Edit::Remove;
  int nodeEditResult=-ENOTSUP;
  bool adminKeysOkay=true,passkeyOkay=true;

  bool ready() const override { return true; }
  int hardwarePinInventory(meshtastic_NodeRemoteHardwarePinsResponse &out)const override {
    out={};out.node_remote_hardware_pins_count=1;out.node_remote_hardware_pins[0].node_num=0x1234;
    out.node_remote_hardware_pins[0].has_pin=true;
    out.node_remote_hardware_pins[0].pin.gpio_pin=3;return 0;
  }
  int save(const mt::Config &) override { ++saveCalls; return -EIO; }
  int editNode(uint32_t id,mt::NodeCache::Edit op,uint32_t) override {
    ++localNodeEditCalls;++nodeEditCalls;lastEditedNode=id;lastNodeEdit=op;return nodeEditResult;
  }
  int editRemoteFavorite(uint32_t id,bool favorite,uint32_t) override {
    ++remoteFavoriteCalls;++nodeEditCalls;lastEditedNode=id;
    lastNodeEdit=favorite?mt::NodeCache::Edit::Favorite:mt::NodeCache::Edit::Unfavorite;
    return nodeEditResult;
  }
  bool send(const uint8_t *bytes,size_t size) override {
    ++sendCalls; (void)bytes; (void)size; return false;
  }
  bool adminKeys(meshtastic_Config_SecurityConfig &out) const override {
    ++const_cast<AdminHost *>(this)->adminKeyReads;
    out=security;
    return adminKeysOkay;
  }
  int saveAdminKeys(const meshtastic_Config_SecurityConfig &,uint32_t) override {
    ++saveAdminKeyCalls; return 0;
  }
  bool adminPasskey(uint8_t *out,size_t size) override {
    ++passkeyCalls;
    if(!passkeyOkay||!out||size!=8)return false;
    memset(out,0x5A,size);
    return true;
  }
  bool networkConfig(meshtastic_Config_NetworkConfig &out) const override {
    ++const_cast<AdminHost *>(this)->networkReads;
    out=network;
    return true;
  }
  uint32_t supportedModules() const override {
    return meshtastic_ExcludedModules_MQTT_CONFIG;
  }
  bool moduleConfig(meshtastic_ModuleConfig &out,unsigned tag) const override {
    ++const_cast<AdminHost *>(this)->moduleReads;
    if(tag!=meshtastic_ModuleConfig_mqtt_tag)return false;
    out.which_payload_variant=meshtastic_ModuleConfig_mqtt_tag;
    auto &mqtt=out.payload_variant.mqtt;
    mqtt.enabled=true;
    strcpy(mqtt.address,"mqtt.synthetic.invalid");
    strcpy(mqtt.username,"synthetic-user");
    strcpy(mqtt.password,"synthetic-mqtt-secret");
    mqtt.encryption_enabled=true;
    mqtt.tls_enabled=true;
    return true;
  }
};

using Wire=Bytes;

Wire getRequest(const meshtastic_AdminMessage &admin) {
  uint8_t payload[MaxPayloadBytes]{};
  auto adminOut=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&adminOut,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};
  data.portnum=meshtastic_PortNum_ADMIN_APP;
  data.has_bitfield=true;
  data.bitfield=2;  // Stock Router's want_response bit; config_ok_to_mqtt is clear.
  data.want_response=true;
  data.payload.size=adminOut.bytes_written;
  memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[MaxDataBytes]{};
  auto dataOut=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&dataOut,meshtastic_Data_fields,&data));
  return {wire,wire+dataOut.bytes_written};
}

meshtastic_AdminMessage ownerGet() {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_owner_request_tag;
  admin.get_owner_request=true;
  return admin;
}

meshtastic_AdminMessage configGet(meshtastic_AdminMessage_ConfigType type) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  admin.get_config_request=type;
  return admin;
}

meshtastic_AdminMessage moduleGet(meshtastic_AdminMessage_ModuleConfigType type) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_module_config_request_tag;
  admin.get_module_config_request=type;
  return admin;
}

meshtastic_AdminMessage channelGet(uint32_t selector) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_channel_request_tag;
  admin.get_channel_request=selector;
  return admin;
}

struct Fixture {
  Key local{11},peer{97};
  Memory identityMemory,ledgerMemory;
  MonocypherIdentityDeriver deriver;
  IdentityJournal identity;
  PkiLedger ledger;
  Gate gate;
  Entropy entropy;
  SignEntropy signing;
  Backend backend;
  mt::experimental::PkiCrypto crypto{backend};
  LedgerNonceAuthority nonce{identity,ledger,ledgerMemory,entropy,gate};
  LedgerReplayAuthority replay{identity,ledger,ledgerMemory,gate};
  PkiRuntime runtime;
  Radio radio;
  mt::Service service{radio,true,local.binding.node,127,true,false,&runtime};
  AdminHost host;
  mt::companion::CompanionWorkspace workspace;
  mt::Companion companion{service,host,meshtastic_HardwareModel_TLORA_V2_1_1P6,workspace};
  mt::Config originalConfig{};
  unsigned originalLedgerWrites=0;
  unsigned originalEncrypts=0,originalDecrypts=0;

  explicit Fixture(bool bindProvider=false):runtime(crypto,backend,identity,nonce,replay,&signing) {
    assert(!identity.load(identityMemory,deriver));
    IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(identityMemory,deriver,candidate,true));
    assert(!ledger.load(ledgerMemory));
    assert(!ledger.initialize(ledgerMemory,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(ledgerMemory,peer.binding.node,
                                  peer.binding.publicKey,true));
    assert(runtime.open(local.binding.node,peer.binding.node));
    gate.service=&service;
    if(bindProvider) {
    assert(service.bindPrivateAdminSnapshot(
        [](void *ctx,uint32_t local,uint32_t from,const uint8_t key[32],uint32_t id,
           const uint8_t *request,size_t requestSize,uint8_t *out,size_t capacity,
           size_t &size)->bool {
          return static_cast<mt::Companion *>(ctx)->privateAdminReplyData(
            local,from,key,id,request,requestSize,out,capacity,size,
            mt::Companion::WorkspaceProof{true});
        },&companion));
    }

    mt::Config config;
    config.bytes[4]=1;
    auto node=config.node();
    node.setPublic();
    config.node(node);
    assert(service.apply(config));
    assert(service.start(false,0));
    assert(service.armEndpoint(true));

    host.network.wifi_enabled=true;
    strcpy(host.network.wifi_ssid,"synthetic-network");
    strcpy(host.network.wifi_psk,"synthetic-wifi-secret");
    strcpy(host.network.ntp_server,"synthetic.ntp.invalid");
    host.network.eth_enabled=true;
    host.security.public_key.size=32;
    memset(host.security.public_key.bytes,0x42,32);
    host.security.private_key.size=32;
    memset(host.security.private_key.bytes,0xA5,32);
    host.security.admin_key_count=1;
    host.security.admin_key[0].size=32;
    memcpy(host.security.admin_key[0].bytes,peer.binding.publicKey,32);

    originalConfig=service.config;
    originalLedgerWrites=ledgerMemory.writes;
    originalEncrypts=backend.encrypts;
    originalDecrypts=backend.decrypts;
  }

  bool reply(const Wire &request,uint32_t requestId,uint32_t from,
             const uint8_t key[32],uint8_t *out,size_t capacity,size_t &size) {
    return companion.privateAdminReplyData(local.binding.node,from,key,requestId,
        request.data(),request.size(),out,capacity,size,
        mt::Companion::WorkspaceProof{true});
  }

  void assertReadOnly() const {
    assert(!memcmp(originalConfig.bytes,service.config.bytes,sizeof(originalConfig.bytes)));
    assert(ledgerMemory.writes==originalLedgerWrites);
    assert(backend.encrypts==originalEncrypts&&backend.decrypts==originalDecrypts);
    assert(host.saveCalls==0&&host.saveAdminKeyCalls==0&&host.sendCalls==0);
    assert(radio.sends==0&&radio.frames.empty());
  }
};

void authorizePeer(AdminHost &host,const Key &peer) {
  host.security.admin_key_count=1;
  host.security.admin_key[0].size=32;
  memcpy(host.security.admin_key[0].bytes,peer.binding.publicKey,32);
}

bool allByte(const uint8_t *bytes,size_t size,uint8_t expected) {
  for(size_t i=0;i<size;++i)if(bytes[i]!=expected)return false;
  return true;
}

meshtastic_AdminMessage decodeReply(const Fixture &fixture,const Wire &request,
                                     uint32_t requestId,const uint8_t *wire,
                                     size_t wireSize) {
  Query query{};
  assert(mt::admin::remote_protocol::canonicalGetRequest(
      request.data(),request.size(),query));
  assert(mt::admin::remote_protocol::canonicalGetResponse(
      wire,wireSize,query,requestId));
  meshtastic_Data data{};
  auto input=pb_istream_from_buffer(wire,wireSize);
  assert(pb_decode(&input,meshtastic_Data_fields,&data)&&!input.bytes_left);
  assert(data.portnum==meshtastic_PortNum_ADMIN_APP&&!data.want_response&&
         data.request_id==requestId);
  meshtastic_AdminMessage admin{};
  auto adminInput=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
  assert(pb_decode(&adminInput,meshtastic_AdminMessage_fields,&admin)&&!adminInput.bytes_left);
  assert(admin.session_passkey.size==8&&allByte(admin.session_passkey.bytes,8,0x5A));
  (void)fixture;
  return admin;
}

void deniedInputsDoNotGeneratePasskeyOrWriteOutput() {
  Fixture f;
  f.host.security.admin_key_count=0;
  const auto request=getRequest(ownerGet());
  uint8_t output[MaxDataBytes];size_t written=0;
  memset(output,0xCC,sizeof(output));
  assert(!f.reply(request,101,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&allByte(output,sizeof(output),0xCC));
  assert(f.host.adminKeyReads==1&&f.host.passkeyCalls==0);

  // A pinned private-message peer is not an Admin unless its key is in this
  // live, explicit Admin allowlist.
  f.host.security.admin_key_count=1;
  f.host.security.admin_key[0].size=32;
  memset(f.host.security.admin_key[0].bytes,0x73,32);
  f.host.adminKeyReads=0;
  assert(!f.reply(request,102,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&allByte(output,sizeof(output),0xCC));
  assert(f.host.adminKeyReads==1&&f.host.passkeyCalls==0);

  // Unknown identity cannot gain authority by choosing the same Admin id.
  authorizePeer(f.host,f.peer);
  f.host.adminKeyReads=0;
  Key unknown{55};
  assert(!f.reply(request,103,unknown.binding.node,unknown.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&allByte(output,sizeof(output),0xCC));
  assert(f.host.passkeyCalls==0);
  f.assertReadOnly();
}

void malformedLiveAllowlistFailsBeforePasskeyOrOutput() {
  Fixture f;
  const auto request=getRequest(ownerGet());
  uint8_t output[MaxDataBytes];size_t written=99;
  memset(output,0xC4,sizeof(output));
  f.host.security.admin_key_count=4;
  assert(!f.reply(request,110,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&allByte(output,sizeof(output),0xC4)&&f.host.passkeyCalls==0);
  f.host.security.admin_key_count=1;
  f.host.security.admin_key[0].size=31;
  assert(!f.reply(request,111,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&allByte(output,sizeof(output),0xC4)&&f.host.passkeyCalls==0);
  f.assertReadOnly();
}

void permittedGetterResponsesAreCorrelatedAndRedacted() {
  Fixture f;
  uint8_t output[MaxDataBytes]{};size_t written=0;
  auto ownerRequest=getRequest(ownerGet());
  assert(f.reply(ownerRequest,120,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  auto owner=decodeReply(f,ownerRequest,120,output,written);
  assert(owner.which_payload_variant==meshtastic_AdminMessage_get_owner_response_tag);
  assert(owner.get_owner_response.hw_model==meshtastic_HardwareModel_TLORA_V2_1_1P6);

  auto deviceRequest=getRequest(configGet(meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG));
  written=0;
  assert(f.reply(deviceRequest,121,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  auto device=decodeReply(f,deviceRequest,121,output,written);
  assert(device.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&
         device.get_config_response.which_payload_variant==meshtastic_Config_device_tag);

  auto deviceUiRequest=getRequest(configGet(
      meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG));
  written=0;
  assert(f.reply(deviceUiRequest,125,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  auto deviceUi=decodeReply(f,deviceUiRequest,125,output,written);
  const meshtastic_DeviceUIConfig emptyDeviceUi{};
  assert(deviceUi.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&
         deviceUi.get_config_response.which_payload_variant==meshtastic_Config_device_ui_tag&&
         !memcmp(&deviceUi.get_config_response.payload_variant.device_ui,&emptyDeviceUi,
                 sizeof(emptyDeviceUi)));

  auto networkRequest=getRequest(configGet(meshtastic_AdminMessage_ConfigType_NETWORK_CONFIG));
  written=0;
  assert(f.reply(networkRequest,122,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  auto network=decodeReply(f,networkRequest,122,output,written);
  const auto &net=network.get_config_response.payload_variant.network;
  assert(network.get_config_response.which_payload_variant==meshtastic_Config_network_tag&&
         !strcmp(net.wifi_psk,"sekrit")&&net.wifi_enabled&&net.eth_enabled&&
         !strcmp(net.wifi_ssid,"synthetic-network")&&
         !strcmp(net.ntp_server,"synthetic.ntp.invalid"));

  auto securityRequest=getRequest(configGet(meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG));
  written=0;
  assert(f.reply(securityRequest,123,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  auto security=decodeReply(f,securityRequest,123,output,written);
  const auto &sec=security.get_config_response.payload_variant.security;
  assert(security.get_config_response.which_payload_variant==meshtastic_Config_security_tag&&
         sec.private_key.size==0&&allByte(sec.private_key.bytes,sizeof(sec.private_key.bytes),0)&&
         sec.public_key.size==32&&sec.admin_key_count==1&&
         sec.admin_key[0].size==32&&
         !memcmp(sec.admin_key[0].bytes,f.peer.binding.publicKey,32));

  auto mqttRequest=getRequest(moduleGet(meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG));
  written=0;
  assert(f.reply(mqttRequest,124,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  auto mqtt=decodeReply(f,mqttRequest,124,output,written);
  const auto &mqttConfig=mqtt.get_module_config_response.payload_variant.mqtt;
  assert(mqtt.get_module_config_response.which_payload_variant==meshtastic_ModuleConfig_mqtt_tag&&
         !strcmp(mqttConfig.password,"sekrit")&&mqttConfig.enabled&&mqttConfig.tls_enabled&&
         !strcmp(mqttConfig.address,"mqtt.synthetic.invalid")&&
         !strcmp(mqttConfig.username,"synthetic-user"));

  for(uint32_t selector:{1u,8u}) {
    auto channelRequest=getRequest(channelGet(selector));
    written=0;
    assert(f.reply(channelRequest,130+selector,f.peer.binding.node,
                   f.peer.binding.publicKey,output,sizeof(output),written));
    auto channel=decodeReply(f,channelRequest,130+selector,output,written);
    assert(channel.which_payload_variant==meshtastic_AdminMessage_get_channel_response_tag&&
           channel.get_channel_response.index==static_cast<int32_t>(selector-1)&&
           channel.get_channel_response.has_settings);
  }

  for(auto operation:{Operation::DeviceMetadata,Operation::ConnectionStatus,Operation::HardwarePins}) {
    meshtastic_AdminMessage selector{};
    selector.which_payload_variant=static_cast<pb_size_t>(operation);
    switch(operation){
    case Operation::DeviceMetadata:selector.get_device_metadata_request=true;break;
    case Operation::ConnectionStatus:selector.get_device_connection_status_request=true;break;
    default:selector.get_node_remote_hardware_pins_request=true;break;
    }
    const auto data=getRequest(selector);
    const auto id=200u+uint32_t(operation);
    assert(f.reply(data,id,f.peer.binding.node,f.peer.binding.publicKey,output,sizeof(output),written));
    const auto result=decodeReply(f,data,id,output,written);
    assert(result.which_payload_variant==mt::stock::admin::responseForRequest(selector.which_payload_variant));
    if(operation==Operation::DeviceMetadata)assert(result.get_device_metadata_response.firmware_version[0]);
    if(operation==Operation::HardwarePins)assert(result.get_node_remote_hardware_pins_response.node_remote_hardware_pins[0].pin.gpio_pin==3);
    if(operation==Operation::ConnectionStatus)assert(!result.get_device_connection_status_response.bluetooth.pin);
  }

  // Authorized getters in the same peer/key session reuse one token.
  assert(f.host.passkeyCalls==1&&f.host.saveAdminKeyCalls==0&&
         f.host.sendCalls==0&&f.host.saveCalls==0&&f.radio.sends==0);
  f.assertReadOnly();
}

void malformedRequestsAndPasskeyFailuresProduceNoReply() {
  Fixture f(true);
  authorizePeer(f.host,f.peer);
  uint8_t output[MaxDataBytes];size_t written=77;
  memset(output,0xD2,sizeof(output));
  // Remote-hardware inventory is now an admitted read-only GET. The first
  // value beyond the current schema remains rejected before authorization.
  const Wire unsupported=getRequest(moduleGet(static_cast<meshtastic_AdminMessage_ModuleConfigType>(
      unsigned(meshtastic_AdminMessage_ModuleConfigType_MESHBEACON_CONFIG)+1u)));
  assert(!f.reply(unsupported,130,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&f.host.adminKeyReads==0&&f.host.passkeyCalls==0&&
         allByte(output,sizeof(output),0xD2));

  const Wire setter=getRequest([]{meshtastic_AdminMessage value{};
    value.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
    strcpy(value.set_owner.long_name,"synthetic setter");return value;}());
  assert(!f.reply(setter,131,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&f.host.adminKeyReads==0&&f.host.passkeyCalls==0&&
         allByte(output,sizeof(output),0xD2));

  const Wire badChannel=getRequest(channelGet(9));
  assert(!f.reply(badChannel,132,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&f.host.adminKeyReads==0&&f.host.passkeyCalls==0);

  Wire noncanonical=getRequest(ownerGet());
  noncanonical.push_back(0x78);noncanonical.push_back(0x01);
  assert(!f.reply(noncanonical,133,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&f.host.adminKeyReads==0&&f.host.passkeyCalls==0);

  Wire oversize(MaxDataBytes+1,0);
  assert(!f.reply(oversize,134,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&f.host.adminKeyReads==0&&f.host.passkeyCalls==0);

  // The trusted Service wrapper refuses an oversized output capacity before
  // invoking the Companion provider or generating a passkey.
  // The provider is boot-bound; enter its valid paused context before
  // testing capacity rejection so a missing/late binding cannot mask it.
  assert(f.service.beginPrivateAdministration());
  size_t oversizeWritten=55;
  uint8_t wideOutput[MaxDataBytes+1];memset(wideOutput,0xB3,sizeof(wideOutput));
  assert(!f.service.privateAdminReplyData(f.local.binding.node,f.peer.binding.node,
      f.peer.binding.publicKey,135,unsupported.data(),unsupported.size(),wideOutput,
      sizeof(wideOutput),oversizeWritten));
  assert(oversizeWritten==0&&f.host.adminKeyReads==0&&f.host.passkeyCalls==0&&
         allByte(wideOutput,sizeof(wideOutput),0xB3));
  assert(f.service.endPrivateAdministration());

  const auto valid=getRequest(ownerGet());
  authorizePeer(f.host,f.peer);
  f.host.passkeyOkay=false;
  memset(output,0xE1,sizeof(output));written=99;
  assert(!f.reply(valid,136,f.peer.binding.node,f.peer.binding.publicKey,
                  output,sizeof(output),written));
  assert(written==0&&f.host.passkeyCalls==1&&allByte(output,sizeof(output),0xE1));
  f.assertReadOnly();
}


void sharedWorkspacePreservesPerTransportParserAndAllowsSequentialReuse() {
  Fixture f;
  mt::Companion second(f.service,f.host,meshtastic_HardwareModel_TLORA_V2_1_1P6,f.workspace,1);
  const auto proof=mt::Companion::WorkspaceProof{true};

  // A partial USB prefix stays in the first Companion while the second uses
  // shared FromRadio scratch to complete an independent handshake.
  f.companion.byte(0x94,1,0,proof);
  meshtastic_ToRadio hello{};
  hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
  hello.want_config_id=17;
  uint8_t body[512]{};auto encoded=pb_ostream_from_buffer(body,sizeof(body));
  assert(pb_encode(&encoded,meshtastic_ToRadio_fields,&hello));
  const size_t length=encoded.bytes_written;
  const uint8_t prefix[4]={0x94,0xc3,uint8_t(length>>8),uint8_t(length)};
  for(uint8_t b:prefix)second.byte(b,2,0,proof);
  for(size_t i=0;i<length;++i)second.byte(body[i],2,0,proof);
  assert(second.handshakes==1&&f.companion.handshakes==0);
  f.companion.byte(0xc3,3,0,proof);f.companion.byte(0,3,0,proof);f.companion.byte(0,3,0,proof);
  assert(f.companion.parseErrors==1&&second.parseErrors==0);

  // Sequential sensitive snapshot callbacks from two Companion instances
  // borrow the same scratch, and each can still complete after the other.
  authorizePeer(f.host,f.peer);
  const auto request=getRequest(ownerGet());
  uint8_t first[MaxDataBytes]{},next[MaxDataBytes]{};size_t firstSize=0,nextSize=0;
  assert(f.reply(request,190,f.peer.binding.node,f.peer.binding.publicKey,
                 first,sizeof(first),firstSize));
  assert(second.privateAdminReplyData(f.local.binding.node,f.peer.binding.node,
      f.peer.binding.publicKey,191,request.data(),request.size(),next,sizeof(next),
      nextSize,proof));
  assert(f.reply(request,192,f.peer.binding.node,f.peer.binding.publicKey,
                 first,sizeof(first),firstSize));
  assert(firstSize&&nextSize);
}

void failedWorkspaceLeaseClearsCallerResultWithoutTouchingLease() {
  Fixture f;
  const auto request=getRequest(ownerGet());
  uint8_t output[MaxDataBytes];memset(output,0xA7,sizeof(output));size_t size=77;
  const bool refused=f.companion.privateAdminReplyData(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,193,request.data(),request.size(),
      output,sizeof(output),size,mt::Companion::WorkspaceProof{false});
  assert(!refused&&size==0&&allByte(output,sizeof(output),0xA7));

  auto lease=f.workspace.acquire(mt::Companion::WorkspaceProof{true});
  assert(lease);
  size=99;
  assert(!f.companion.privateAdminReplyData(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,194,request.data(),request.size(),
      output,sizeof(output),size,mt::Companion::WorkspaceProof{true}));
  assert(size==0&&lease);
}

}  // namespace

int main() {
  deniedInputsDoNotGeneratePasskeyOrWriteOutput();
  malformedLiveAllowlistFailsBeforePasskeyOrOutput();
  permittedGetterResponsesAreCorrelatedAndRedacted();
  malformedRequestsAndPasskeyFailuresProduceNoReply();
  sharedWorkspacePreservesPerTransportParserAndAllowsSequentialReuse();
  failedWorkspaceLeaseClearsCallerResultWithoutTouchingLease();
  std::cout << "PASS direct synthetic Companion snapshot policy, redaction, and no-write controls; no auth provenance or encrypted RF claim\n";
  std::cout << "sizeof AdminMessage=" << sizeof(meshtastic_AdminMessage)
            << " SecurityConfig=" << sizeof(meshtastic_Config_SecurityConfig)
            << " Data=" << sizeof(meshtastic_Data)
            << " Companion=" << sizeof(mt::Companion)
            << " MaxDataBytes=" << MaxDataBytes
            << " MaxPayloadBytes=" << MaxPayloadBytes << "\n";
  constexpr size_t validatorScratchBytes = MaxPayloadBytes * 2 + sizeof(meshtastic_AdminMessage) +
      sizeof(meshtastic_Data) + MaxDataBytes;
  std::cout << "protocol validator scratch object sum per nested GET validator="
            << validatorScratchBytes
            << " bytes (payload scratch + AdminMessage + canonical Admin + nested Data + canonical Data; excludes call frames/compiler spills)\n";
  return 0;
}
