// SPDX-License-Identifier: GPL-3.0-only
// Reuse the adjacent real-PSA fixture and responder regression cases.
#define main m143_private_admin_responder_regressions
#include "m143_private_admin_responder_tests.cpp"
#undef main

namespace {
constexpr uint32_t OtherPeerNode=0x3456789au;
Bytes adminOwnerResponse(uint32_t requestId) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_owner_response_tag;
  admin.get_owner_response=meshtastic_User_init_zero;
  admin.session_passkey.size=8;memset(admin.session_passkey.bytes,0x5a,8);
  uint8_t payload[233]{};auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&ps,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;data.request_id=requestId;
  data.has_bitfield=true;data.payload.size=ps.bytes_written;memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};auto ds=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&ds,meshtastic_Data_fields,&data));return {wire,wire+ds.bytes_written};
}

Bytes adminConfigResponse(uint32_t requestId) {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant=meshtastic_AdminMessage_get_config_response_tag;
  admin.get_config_response.which_payload_variant=meshtastic_Config_device_tag;
  admin.get_config_response.payload_variant.device=meshtastic_Config_DeviceConfig_init_zero;
  admin.session_passkey.size=8;memset(admin.session_passkey.bytes,0x5a,8);
  uint8_t payload[233]{};auto ps=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&ps,meshtastic_AdminMessage_fields,&admin));
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ADMIN_APP;data.request_id=requestId;
  data.has_bitfield=true;data.payload.size=ps.bytes_written;memcpy(data.payload.bytes,payload,data.payload.size);
  uint8_t wire[227]{};auto ds=pb_ostream_from_buffer(wire,sizeof(wire));
  assert(pb_encode(&ds,meshtastic_Data_fields,&data));return {wire,wire+ds.bytes_written};
}

template <typename Signer>
Bytes routingProof(M143Fixture &f,Signer &signer,uint32_t source,
                   uint32_t wireRequest,uint32_t signedRequest,uint32_t code,
                   uint32_t packetId,bool attachProof=true) {
  uint8_t routing[12]{},raw[2]={24,uint8_t(code)};size_t routingSize=sizeof(raw);
  memcpy(routing,raw,sizeof(raw));
  if(attachProof) {
    mt::experimental::PkiAck proof(f.crypto);
    assert(proof.attach(signer.scalar,f.local.binding.publicKey,
      {true,source,f.local.binding.node,signedRequest},raw,sizeof(raw),
      routing,sizeof(routing),routingSize));
  }
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_ROUTING_APP;
  data.request_id=wireRequest;data.has_bitfield=true;data.payload.size=routingSize;
  memcpy(data.payload.bytes,routing,routingSize);
  uint8_t payload[227]{},frame[255]{};
  auto output=pb_ostream_from_buffer(payload,sizeof(payload));
  assert(pb_encode(&output,meshtastic_Data_fields,&data));
  mt::PrimaryChannel::Selection selection{};
  assert(f.service.config.channelSelection(0,selection));
  bench::MtCodec codec;
  assert(codec.setKey(selection.hash,selection.bytes,selection.length));
  mt::PrimaryChannel::clear(selection);
  size_t frameSize=0;
  assert(codec.encodeZeroHopData(source,f.local.binding.node,packetId,payload,
    output.bytes_written,frame,sizeof(frame),frameSize));
  return {frame,frame+frameSize};
}

void transmitPrivateAdminJobs(M143Fixture &f,uint32_t now,unsigned count) {
  for(unsigned i=0;i<count;++i,now+=1000) {
    f.service.poll(now,71);
    assert(f.radio.sends==i+1);
    f.radio.done=true;
    f.service.poll(now+200,71);
  }
}

void adminClientCases() {
  const auto request=adminRequest();
  {
    M143Fixture f;const auto writes=f.lm.writes;const auto entropy=f.entropy.calls;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,120,request.data(),request.size(),
      3,false,nullptr,0,true,1500)==0);
    assert(f.service.submitPrivate(0,f.peer.binding.node,121,request.data(),request.size(),
      3,false,f.peer.binding.publicKey,32,true,1500)==-EINVAL);
    const auto setter=adminSetter();
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,122,setter.data(),setter.size(),
      3,false,nullptr,0,true,1500)==-EINVAL);
    assert(f.lm.writes>writes&&f.entropy.calls>entropy);
    group("dedicated client accepts canonical GET while generic PKI submit keeps Admin closed");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,130,request.data(),request.size(),
      3,false,nullptr,0,true,1500)==0);
    const auto response=adminOwnerResponse(130);
    const auto frame=authenticatedFrame(f,230,response,false);
    f.service.rx(frame.data(),frame.size(),3,1500,81);
    f.bc.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});
    assert(f.usb.out.empty()&&f.ble.out.empty());
    f.c.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});
    assert(f.usb.out.size()==1&&f.ble.out.empty());
    const auto &out=f.usb.out.front();assert(out.which_payload_variant==meshtastic_FromRadio_packet_tag);
    assert(out.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&out.packet.decoded.request_id==130);
    assert(f.usb.previews==0&&f.ble.previews==0);
    f.c.receive(frame.data(),frame.size(),-80,3,1600,mt::Companion::WorkspaceProof{true});
    assert(f.usb.out.size()==1&&f.ble.out.empty());
    PrivateEvent retained{};
    assert(!f.service.takePrivateEvent(0,frame.data(),frame.size(),retained));
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,230)==ReplayCheck::Seen);
    group("correlated response rejects wrong owner, delivers once to requester, then wipes event state");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,135,request.data(),request.size(),
      0,true,nullptr,0,true,1500)==0);
    f.service.poll(2000,71);assert(f.radio.sends==1);
    f.radio.done=true;f.service.poll(2200,71);
    const auto response=authenticatedFrame(f,235,adminOwnerResponse(135),false);
    f.receive(response,2300);
    mt::Endpoint::Result result{};
    assert(f.service.takeMessageResult(0,result)&&result.id==135&&
      result.kind==mt::Endpoint::Kind::PrivateResponse&&result.code==0);
    f.service.poll(40000,71);
    assert(!f.service.takeMessageResult(0,result)&&f.radio.sends==1);
    group("authenticated Admin application reply completes reliable TX without Routing-proof claim or late retry");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,140,request.data(),request.size(),
      3,false,nullptr,0,true,1500)==0);
    const auto wrong=authenticatedFrame(f,240,adminOwnerResponse(141),false);
    f.receive(wrong);
    assert(f.usb.out.empty()&&f.ble.out.empty());
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,240)==ReplayCheck::Fresh);
    const auto malformed=authenticatedFrame(f,241,adminConfigResponse(140),false);
    f.receive(malformed,1600);
    assert(f.usb.out.empty()&&f.ble.out.empty());
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,241)==ReplayCheck::Fresh);
    const auto correct=authenticatedFrame(f,242,adminOwnerResponse(140),false);
    f.receive(correct,1700);
    assert(f.usb.out.size()==1&&f.ble.out.empty());
    group("wrong request correlation and wrong Admin response variant are rejected without replay commit");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,150,request.data(),request.size(),
      3,false,nullptr,0,true,1500)==0);
    const auto response=authenticatedFrame(f,250,adminOwnerResponse(150),false);
    const auto persisted=f.lm.values;
    f.lm.fault=1;f.receive(response);
    assert(f.usb.out.empty()&&f.ble.out.empty());
    assert(f.lm.values==persisted&&!f.runtime.ready(f.local.binding.node)&&
      f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,250)==ReplayCheck::NotReady);
    group("failed durable replay write freezes the provider and suppresses the Admin response");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,160,request.data(),request.size(),
      3,true,nullptr,0,true,1500)==0);
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,161,request.data(),request.size(),
      3,true,nullptr,0,true,1501)==0);
    transmitPrivateAdminJobs(f,2000,2);

    const auto rejected=routingProof(f,f.peer,f.peer.binding.node,160,160,
      meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED,1160);
    f.service.rx(rejected.data(),rejected.size(),-70,4500,82);
    mt::Endpoint::Result result{};
    assert(f.service.takeMessageResult(0,result)&&result.id==160&&
      result.kind==mt::Endpoint::Kind::Authenticated&&
      result.code==meshtastic_Routing_Error_ADMIN_PUBLIC_KEY_UNAUTHORIZED);
    assert(!f.service.takeMessageResult(0,result));

    const auto lateRejected=authenticatedFrame(f,260,adminOwnerResponse(160),false);
    f.receive(lateRejected,4600);
    assert(f.usb.out.empty()&&f.ble.out.empty());
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,260)==ReplayCheck::Fresh);
    const auto sibling=authenticatedFrame(f,261,adminOwnerResponse(161),false);
    f.receive(sibling,4700);
    assert(f.usb.out.size()==1&&f.ble.out.empty());
    assert(f.usb.out[0].which_payload_variant==meshtastic_FromRadio_packet_tag&&
      f.usb.out[0].packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&
      f.usb.out[0].packet.decoded.request_id==161);
    assert(f.usb.previews==0&&f.ble.previews==0);
    group("verified nonzero Routing proof cancels only its Admin query without synthesizing an app response");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,170,request.data(),request.size(),
      3,true,nullptr,0,true,1500)==0);
    transmitPrivateAdminJobs(f,2000,1);

    const auto positive=routingProof(f,f.peer,f.peer.binding.node,170,170,0,1170);
    f.service.rx(positive.data(),positive.size(),-70,3300,83);
    mt::Endpoint::Result result{};
    assert(f.service.takeMessageResult(0,result)&&result.id==170&&
      result.kind==mt::Endpoint::Kind::Authenticated&&result.code==0);
    assert(!f.service.takeMessageResult(0,result));
    const auto response=authenticatedFrame(f,270,adminOwnerResponse(170),false);
    f.receive(response,3400);
    assert(f.usb.out.size()==1&&f.ble.out.empty()&&
      f.usb.out[0].packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&
      f.usb.out[0].packet.decoded.request_id==170);
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,270)==ReplayCheck::Seen);
    group("verified successful Routing ACK leaves Admin query pending for the correlated application response");
  }
  {
    M143Fixture f;
    assert(f.service.submitPrivateAdminGet(0,f.peer.binding.node,180,request.data(),request.size(),
      3,true,nullptr,0,true,1500)==0);
    transmitPrivateAdminJobs(f,2000,1);

    Key wrongSigner{0x52};
    const auto wrongPeer=routingProof(f,f.peer,OtherPeerNode,180,180,37,1180);
    f.service.rx(wrongPeer.data(),wrongPeer.size(),-70,3300,84);
    const auto wrongKey=routingProof(f,wrongSigner,f.peer.binding.node,180,180,37,1181);
    f.service.rx(wrongKey.data(),wrongKey.size(),-70,3400,85);
    const auto wrongId=routingProof(f,f.peer,f.peer.binding.node,180,181,37,1182);
    f.service.rx(wrongId.data(),wrongId.size(),-70,3500,86);
    const auto unsignedError=routingProof(f,f.peer,f.peer.binding.node,180,180,37,1183,false);
    f.service.rx(unsignedError.data(),unsignedError.size(),-70,3600,87);
    mt::Endpoint::Result result{};
    assert(!f.service.takeMessageResult(0,result));

    const auto stillCorrelated=authenticatedFrame(f,280,adminOwnerResponse(180),false);
    f.receive(stillCorrelated,3700);
    assert(f.usb.out.size()==1&&f.ble.out.empty()&&
      f.usb.out[0].packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP&&
      f.usb.out[0].packet.decoded.request_id==180);
    assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,280)==ReplayCheck::Seen);
    group("wrong-peer, wrong-key, wrong-request and unsigned Routing errors cannot cancel a pending Admin query");
  }
}
}

int main() {
  m143_private_admin_responder_regressions();
  adminClientCases();
}
