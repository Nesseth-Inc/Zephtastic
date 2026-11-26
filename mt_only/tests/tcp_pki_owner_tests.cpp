// SPDX-License-Identifier: GPL-3.0-only
#include "pki_runtime_fixture.h"
static unsigned authenticated(const Host&host,uint32_t request){unsigned n=0;for(const auto&v:host.out)if(v.which_payload_variant==meshtastic_FromRadio_packet_tag&&v.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&v.packet.decoded.request_id==request&&v.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_VALID)++n;return n;}
int main(){
 static_assert(mt::LocalClientCount==3);
 {
  Fixture f;Host host;mt::companion::CompanionWorkspace workspace;mt::Companion tcp(f.s,host,81,workspace,2);meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=3;input(tcp,hello,1);host.out.clear();
  auto request=f.request();input(tcp,request);const auto entropy=f.entropy.calls,encrypts=f.backend.encrypts;assert(entropy==1&&f.ledger.sent(10));input(f.c,request);input(f.bc,request);assert(f.entropy.calls==entropy&&f.backend.encrypts==encrypts&&f.s.endpointFree()==3);
  f.poll();f.complete();f.rx(f.ack());f.poll(1600);tcp.poll(mt::Companion::WorkspaceProof{true});assert(f.radio.sends==1&&authenticated(f.usb,10)==1&&authenticated(f.ble,10)==1&&authenticated(host,10)==1);
  puts("PASS real PSA ciphertext shares one durable nonce/job with independent USB BLE TCP proof results");
 }
 for(bool tcpBeforeBle:{false,true}){
  Fixture f;Host host;mt::companion::CompanionWorkspace workspace;mt::Companion tcp(f.s,host,81,workspace,2);
  meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=3;input(tcp,hello,1);host.out.clear();
  auto frame=f.incoming(91,"three clients",false);const auto decrypts=f.backend.decrypts;
  f.s.rx(frame.data(),frame.size(),3,1500,81);f.c.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});
  if(tcpBeforeBle){tcp.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});f.bc.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});}
  else{f.bc.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});tcp.receive(frame.data(),frame.size(),-80,3,1500,mt::Companion::WorkspaceProof{true});}
  f.c.receive(frame.data(),frame.size(),-80,3,1501,mt::Companion::WorkspaceProof{true});f.bc.receive(frame.data(),frame.size(),-80,3,1501,mt::Companion::WorkspaceProof{true});tcp.receive(frame.data(),frame.size(),-80,3,1501,mt::Companion::WorkspaceProof{true});
  assert(f.backend.decrypts==decrypts+1&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1&&f.delivered(host)==1);
  mt::pki::PrivateEvent event;for(uint8_t owner:{0,1,2,3})assert(!f.runtime.event(owner,frame.data(),frame.size(),event));
  puts(tcpBeforeBle?"PASS real PSA fanout order021 exactly once per connected owner":"PASS real PSA fanout main order012 exactly once per connected owner");
 }
}
