// SPDX-License-Identifier: GPL-3.0-only
// Exercises the real Companion mutation dispatch with the existing synthetic
// authorized-session fixture. The Host setter is a recorder, not durable-store proof.
#define main m149_snapshot_fixture_unused_main
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

Wire favoriteSet(const uint8_t token[8],uint32_t node,bool set) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=set?meshtastic_AdminMessage_set_favorite_node_tag:
                                  meshtastic_AdminMessage_remove_favorite_node_tag;
  if(set)admin.set_favorite_node=node;else admin.remove_favorite_node=node;
  admin.session_passkey.size=8;memcpy(admin.session_passkey.bytes,token,8);
  return getRequest(admin);
}

void testFavoriteAndUnfavoriteUseExactTypedSetter() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  constexpr uint32_t target=0x12345678u;
  f.host.nodeEditResult=0;
  for(bool set:{true,false}) {
    const Wire request=favoriteSet(token,target,set);uint32_t error=UINT32_MAX;
    assert(f.companion.privateAdminMutation(f.local.binding.node,f.peer.binding.node,
      f.peer.binding.publicKey,set?911:912,request.data(),request.size(),0,error,
      mt::Companion::WorkspaceProof{true}));
    assert(error==meshtastic_Routing_Error_NONE);
    assert(f.host.nodeEditCalls==(set?1u:2u));
    assert(f.host.remoteFavoriteCalls==(set?1u:2u)&&f.host.localNodeEditCalls==0);
    assert(f.host.lastEditedNode==target);
    assert(f.host.lastNodeEdit==(set?mt::NodeCache::Edit::Favorite:
                                     mt::NodeCache::Edit::Unfavorite));
  }
  assert(f.host.localNodeEditCalls==0);
  assert(f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testInvalidAndSelfTargetsNeverProduceSuccess() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  uint32_t error=UINT32_MAX;
  for(uint32_t invalid:{0u,1u,0xfffffffeu,0xffffffffu}) {
    const Wire request=favoriteSet(token,invalid,true);
    assert(!f.companion.privateAdminMutation(f.local.binding.node,f.peer.binding.node,
      f.peer.binding.publicKey,920+invalid,request.data(),request.size(),0,error,
      mt::Companion::WorkspaceProof{true}));
    assert(error==meshtastic_Routing_Error_BAD_REQUEST&&f.host.nodeEditCalls==0);
  }
  const Wire self=favoriteSet(token,f.local.binding.node,true);
  f.host.nodeEditResult=-EINVAL;
  assert(f.companion.privateAdminMutation(f.local.binding.node,f.peer.binding.node,
    f.peer.binding.publicKey,921,self.data(),self.size(),0,error,
    mt::Companion::WorkspaceProof{true}));
  assert(error==meshtastic_Routing_Error_BAD_REQUEST&&f.host.nodeEditCalls==1);
  assert(f.host.localNodeEditCalls==0);
  assert(f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}

void testSetterFailureMapsToNoInterface() {
  Fixture f;authorizePeer(f.host,f.peer);
  uint8_t token[8]{};issueTokenThroughAuthorizedGetter(f,token);
  const Wire request=favoriteSet(token,0x12345678u,true);uint32_t error=UINT32_MAX;
  f.host.nodeEditResult=-EIO;
  assert(f.companion.privateAdminMutation(f.local.binding.node,f.peer.binding.node,
    f.peer.binding.publicKey,930,request.data(),request.size(),0,error,
    mt::Companion::WorkspaceProof{true}));
  assert(error==meshtastic_Routing_Error_NO_INTERFACE&&f.host.nodeEditCalls==1);
  assert(f.host.localNodeEditCalls==0);
  assert(f.host.sendCalls==0&&f.radio.sends==0);
  bench::wipeKeyBuffer(token,sizeof(token));
}
} // namespace

int main() {
  testFavoriteAndUnfavoriteUseExactTypedSetter();
  testInvalidAndSelfTargetsNeverProduceSuccess();
  testSetterFailureMapsToNoInterface();
  puts("PASS_M149_FAVORITE_COMPANION_DISPATCH_SYNTHETIC");
  return 0;
}
