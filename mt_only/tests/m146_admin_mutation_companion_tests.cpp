// Synthetic callback tests for the opt-in authenticated private Admin SET path.
// Fixtures do not prove radio delivery or target persistence implementation.
#define main m143_admin_snapshot_unused_main
#include "m143_admin_snapshot_tests.cpp"
#undef main

namespace {

void issueTokenThroughAuthorizedGetter(Fixture &f,uint8_t token[8]) {
  const auto request=getRequest(ownerGet());
  uint8_t output[MaxDataBytes]{};size_t written=0;
  assert(f.reply(request,900,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  meshtastic_Data data{};
  auto in=pb_istream_from_buffer(output,written);
  assert(pb_decode(&in,meshtastic_Data_fields,&data)&&!in.bytes_left);
  meshtastic_AdminMessage response{};
  in=pb_istream_from_buffer(data.payload.bytes,data.payload.size);
  assert(pb_decode(&in,meshtastic_AdminMessage_fields,&response)&&!in.bytes_left);
  assert(response.session_passkey.size==8);
  memcpy(token,response.session_passkey.bytes,8);
  bench::wipeKeyBuffer(&response,sizeof(response));
}

Wire ownerSet(const uint8_t token[8]) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  strcpy(admin.set_owner.long_name,"Remote owner name");
  admin.session_passkey.size=8;
  memcpy(admin.session_passkey.bytes,token,8);
  return getRequest(admin);
}

Wire authorizedSet(meshtastic_AdminMessage admin,const uint8_t token[8]) {
  admin.session_passkey.size=8;
  memcpy(admin.session_passkey.bytes,token,8);
  return getRequest(admin);
}

uint32_t applyAuthorizedSet(Fixture &f,const Wire &request,uint32_t id) {
  uint32_t error=UINT32_MAX;
  assert(f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,id,request.data(),request.size(),
      1000,error,mt::Companion::WorkspaceProof{true}));
  return error;
}

meshtastic_AdminMessage getAuthorizedConfig(Fixture &f,
    meshtastic_AdminMessage_ConfigType type,uint32_t requestId) {
  const Wire request=getRequest(configGet(type));
  uint8_t output[MaxDataBytes]{};size_t written=0;
  assert(f.reply(request,requestId,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  return decodeReply(f,request,requestId,output,written);
}

meshtastic_AdminMessage getAuthorizedChannel(Fixture &f,uint32_t selector,
                                               uint32_t requestId) {
  const Wire request=getRequest(channelGet(selector));
  uint8_t output[MaxDataBytes]{};size_t written=0;
  assert(f.reply(request,requestId,f.peer.binding.node,f.peer.binding.publicKey,
                 output,sizeof(output),written));
  return decodeReply(f,request,requestId,output,written);
}

meshtastic_AdminMessage channelSet(const meshtastic_Channel &channel,
                                   const uint8_t token[8]) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_channel_tag;
  admin.set_channel=channel;
  admin.session_passkey.size=8;
  memcpy(admin.session_passkey.bytes,token,8);
  return admin;
}

void testRemoteChannelFalseConsentRequiresReadyMqttOwner() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  const auto response=getAuthorizedChannel(f,1,911);
  assert(response.which_payload_variant==meshtastic_AdminMessage_get_channel_response_tag);
  auto channel=response.get_channel_response;
  // The getter redacts this public primary PSK. Restore its canonical known
  // public value so SET remains a valid unchanged channel projection.
  channel.settings.psk.size=sizeof(bench::MtNodeRecord::PublicKey);
  memcpy(channel.settings.psk.bytes,bench::MtNodeRecord::PublicKey,
         sizeof(bench::MtNodeRecord::PublicKey));
  const Wire request=getRequest(channelSet(channel,token));
  assert(applyAuthorizedSet(f,request,912)==meshtastic_Routing_Error_NO_INTERFACE);
  assert(f.service.config.same(f.originalConfig)&&f.host.saveCalls==0&&
         f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

// AdminHost inherits the production Host defaults for optional RF owners:
// mqttAvailable() is false and the typed remote-save hooks return -ENOTSUP.
// This existing fixture proves fail-closed errors and no core-save fallback,
// but exposes no per-hook counters and does not validate Main's concrete owner.
void testChangedChannelFalseConsentNeverFallsBackToCoreSave() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  const auto response=getAuthorizedChannel(f,1,921);
  assert(response.which_payload_variant==meshtastic_AdminMessage_get_channel_response_tag);
  auto channel=response.get_channel_response;
  channel.role=meshtastic_Channel_Role_PRIMARY;
  channel.has_settings=true;
  strcpy(channel.settings.name,"ChangedLink");
  channel.settings.psk.size=sizeof(bench::MtNodeRecord::PublicKey);
  memcpy(channel.settings.psk.bytes,bench::MtNodeRecord::PublicKey,
         sizeof(bench::MtNodeRecord::PublicKey));
  channel.settings.uplink_enabled=false;
  const Wire request=getRequest(channelSet(channel,token));
  assert(applyAuthorizedSet(f,request,922)==meshtastic_Routing_Error_NO_INTERFACE);
  assert(f.service.config.same(f.originalConfig)&&f.host.saveCalls==0&&
         f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testRemoteLoRaFalseConsentCannotUseUnavailableGetterAsProof() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  const auto response=getAuthorizedConfig(f,
      meshtastic_AdminMessage_ConfigType_LORA_CONFIG,931);
  assert(response.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&
         response.get_config_response.which_payload_variant==meshtastic_Config_lora_tag&&
         !response.get_config_response.payload_variant.lora.config_ok_to_mqtt);
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  admin.set_config=response.get_config_response;
  const Wire request=authorizedSet(admin,token);
  assert(applyAuthorizedSet(f,request,932)==meshtastic_Routing_Error_NO_INTERFACE);
  assert(f.service.config.same(f.originalConfig)&&f.host.saveCalls==0&&
         f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testUnchangedRemoteDeviceStillRequiresHeartbeatOwner() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  const auto response=getAuthorizedConfig(f,
      meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG,941);
  assert(response.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag&&
         response.get_config_response.which_payload_variant==meshtastic_Config_device_tag&&
         response.get_config_response.payload_variant.device.led_heartbeat_disabled);
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  admin.set_config=response.get_config_response;
  const Wire request=authorizedSet(admin,token);
  // An unchanged core Config is not evidence that the heartbeat/notification
  // owner committed; this fixture's concrete remote hook remains unavailable.
  assert(applyAuthorizedSet(f,request,942)==meshtastic_Routing_Error_NO_INTERFACE);
  assert(f.service.config.same(f.originalConfig)&&f.host.saveCalls==0&&
         f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testLocalOwnerEchoDoesNotExpandRemoteSetter() {
  Fixture f;
  authorizePeer(f.host,f.peer);
  uint8_t token[8]{};
  issueTokenThroughAuthorizedGetter(f,token);
  meshtastic_AdminMessage set{};
  set.which_payload_variant=meshtastic_AdminMessage_set_owner_tag;
  snprintf(set.set_owner.id,sizeof(set.set_owner.id),"!%08x",unsigned(f.local.binding.node));
  strcpy(set.set_owner.long_name,"Changed name");
  assert(applyAuthorizedSet(f,authorizedSet(set,token),903)==meshtastic_Routing_Error_BAD_REQUEST);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testAuthorizedMutationUsesExistingTypedSetterAndCache() {
  Fixture f;
  authorizePeer(f.host,f.peer);
  uint8_t token[8]{};
  issueTokenThroughAuthorizedGetter(f,token);
  const Wire request=ownerSet(token);
  uint32_t error=UINT32_MAX;
  const bool handled=f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,901,request.data(),request.size(),0,error,mt::Companion::WorkspaceProof{true});
  assert(handled);
  // The fixture deliberately returns -EIO from save(); reaching that result
  // proves the existing typed owner setter accepted the canonical body.
  assert(error==meshtastic_Routing_Error_BAD_REQUEST&&f.host.saveCalls==1);
  assert(f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testInvalidatedAndWrongTokensCannotReachSetter() {
  Fixture f;
  authorizePeer(f.host,f.peer);
  uint8_t token[8]{};
  issueTokenThroughAuthorizedGetter(f,token);
  const Wire request=ownerSet(token);
  f.companion.clearPrivateAdminSessions();
  uint32_t error=UINT32_MAX;
  assert(f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,902,request.data(),request.size(),0,error,mt::Companion::WorkspaceProof{true}));
  assert(error==meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY&&f.host.saveCalls==0);

  token[0]^=0x80;
  const Wire wrong=ownerSet(token);
  assert(f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,903,wrong.data(),wrong.size(),0,error,mt::Companion::WorkspaceProof{true}));
  assert(error==meshtastic_Routing_Error_ADMIN_BAD_SESSION_KEY&&f.host.saveCalls==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testFailedWorkspaceLeaseDoesNotLeaveStaleMutationResult() {
  Fixture f;
  const Wire malformed{0x08};
  uint32_t error=UINT32_MAX;
  assert(!f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,905,malformed.data(),malformed.size(),
      0,error,mt::Companion::WorkspaceProof{false}));
  assert(error==meshtastic_Routing_Error_BAD_REQUEST&&f.host.saveCalls==0);

  auto lease=f.workspace.acquire(mt::Companion::WorkspaceProof{true});
  assert(lease);error=UINT32_MAX;
  assert(!f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,906,malformed.data(),malformed.size(),
      0,error,mt::Companion::WorkspaceProof{true}));
  assert(error==meshtastic_Routing_Error_BAD_REQUEST&&lease&&f.host.saveCalls==0);
}

void testCurrentAllowlistDenialDoesNotReachSetter() {
  Fixture f;
  authorizePeer(f.host,f.peer);
  uint8_t token[8]{};
  issueTokenThroughAuthorizedGetter(f,token);
  authorizePeer(f.host,f.local); // Remove the requester from the live allowlist.
  const Wire request=ownerSet(token);
  uint32_t error=UINT32_MAX;
  assert(!f.companion.privateAdminMutation(f.local.binding.node,
      f.peer.binding.node,f.peer.binding.publicKey,904,request.data(),request.size(),0,error,mt::Companion::WorkspaceProof{true}));
  assert(f.host.saveCalls==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

} // namespace

int main() {
  testAuthorizedMutationUsesExistingTypedSetterAndCache();
  testLocalOwnerEchoDoesNotExpandRemoteSetter();
  testInvalidatedAndWrongTokensCannotReachSetter();
  testCurrentAllowlistDenialDoesNotReachSetter();
  testFailedWorkspaceLeaseDoesNotLeaveStaleMutationResult();
  testRemoteChannelFalseConsentRequiresReadyMqttOwner();
  testChangedChannelFalseConsentNeverFallsBackToCoreSave();
  testRemoteLoRaFalseConsentCannotUseUnavailableGetterAsProof();
  testUnchangedRemoteDeviceStillRequiresHeartbeatOwner();
  puts("PASS_M146_ADMIN_MUTATION_COMPANION");
  return 0;
}
