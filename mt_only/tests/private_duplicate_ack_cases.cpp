// SPDX-License-Identifier: GPL-3.0-only
// Actual Service/Companion/PSA regression cases. Public deterministic fixtures;
// modeled storage and radio only, never a target-linked file or RF claim.
#include "pki_runtime_fixture.h"
#include "known_channel_fixture.h"

namespace {
void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}
PkiLedger::Status counts(Fixture &f){PkiLedger::Status s;assert(f.ledger.status(s));return s;}
void unchanged(Fixture &f,const PkiLedger::Status &before){
  const auto after=counts(f);
  assert(after.generation==before.generation&&after.sent==before.sent&&after.received==before.received&&
         after.stored==before.stored&&after.active==before.active&&after.format==before.format);
}
void sendAck(Fixture &f,uint32_t now){
  const auto before=f.radio.frames.size();f.poll(now);
  assert(f.radio.frames.size()==before+1);f.complete(now+200);
  assert(f.radio.frames.size()==before+1);
}
void proof(Fixture &f,size_t index,uint32_t request,unsigned hop){
  const auto &wire=f.radio.frames.at(index);bench::RouteMetadata m;
  assert(f.s.codec.decode(wire.data(),wire.size(),m)&&m.portnum==5&&m.request_id==request&&m.ackProofLength==8);
  assert(bench::le32(wire.data())==f.peer.binding.node&&bench::le32(wire.data()+4)==f.local.binding.node);
  assert(bench::le32(wire.data()+8)&&f.ledger.sent(bench::le32(wire.data()+8)));
  assert((wire[12]&7)==hop&&((wire[12]>>5)&7)==hop);
  uint8_t routing[233]{};size_t n=0;mt::experimental::PkiAck ack(f.crypto);
  assert(ack.verify(f.peer.scalar,f.local.binding.publicKey,
    {true,f.local.binding.node,f.peer.binding.node,request},
    {true,bench::le32(wire.data()+4),bench::le32(wire.data()),m.request_id},
    m.payload,m.payloadLength,routing,sizeof(routing),n)==mt::experimental::AckProofVerdict::ValidProof);
  assert(n==2&&routing[0]==24&&routing[1]==0);
}
Bytes telemetryRequest(Fixture &f,uint32_t id){
  meshtastic_Data data{};data.portnum=meshtastic_PortNum_TELEMETRY_APP;
  data.want_response=true;data.has_bitfield=true;data.bitfield=2;
  meshtastic_Telemetry telemetry{};telemetry.which_variant=meshtastic_Telemetry_device_metrics_tag;
  auto payload=pb_ostream_from_buffer(data.payload.bytes,sizeof(data.payload.bytes));
  assert(pb_encode(&payload,meshtastic_Telemetry_fields,&telemetry));data.payload.size=payload.bytes_written;
  uint8_t plain[227]{},cipher[239]{};auto encoded=pb_ostream_from_buffer(plain,sizeof(plain));size_t n=0;
  assert(pb_encode(&encoded,meshtastic_Data_fields,&data));
  assert(f.crypto.encrypt(f.peer.scalar,f.local.binding.publicKey,f.peer.binding.node,id,0x12345678,
    plain,encoded.bytes_written,cipher,sizeof(cipher),n));
  Bytes frame(16+n);put(frame.data(),f.local.binding.node);put(frame.data()+4,f.peer.binding.node);
  put(frame.data()+8,id);frame[12]=8;frame[15]=uint8_t(f.peer.binding.node);memcpy(frame.data()+16,cipher,n);
  return frame;
}
}

int main(){
  {Fixture f;auto original=f.incoming();original[12]=uint8_t(1|8|(3<<5));
    f.rx(original,1500);sendAck(f,2000);proof(f,0,90,3);const auto first=counts(f);
    const auto originalAck=bench::le32(f.radio.frames[0].data()+8);
    auto duplicate=original;duplicate[12]=uint8_t(3|8|(3<<5));f.rx(duplicate,2500);
    const auto repeated=counts(f);
    assert(repeated.received==first.received&&repeated.sent==first.sent+1&&repeated.generation==first.generation+1);
    assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
    sendAck(f,3000);proof(f,1,90,3);
    assert(bench::le32(f.radio.frames[1].data()+8)!=originalAck);
    f.rx(original,3500);unchanged(f,repeated);f.poll(4000);assert(f.radio.frames.size()==2);
    pass("exact authenticated duplicate gets one fresh proof ACK at original hop without RX recommit or redelivery");}
  {Fixture f;const auto original=f.incoming();f.rx(original,1500);sendAck(f,2000);const auto first=counts(f);
    auto tampered=original;tampered[16]^=1;f.rx(tampered,2500);unchanged(f,first);
    f.rx(f.incoming(90,"different authenticated body"),2600);unchanged(f,first);
    auto wrongRoute=original;put(wrongRoute.data()+4,f.peer.binding.node^0x10000);f.rx(wrongRoute,2700);unchanged(f,first);
    f.rx(original,2800);sendAck(f,3300);proof(f,1,90,0);
    assert(counts(f).received==first.received&&counts(f).sent==first.sent+1&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
    pass("bad tag different authentic ciphertext and wrong route cannot consume the exact duplicate ACK intent");}
  {Fixture f;auto original=f.incoming(90,"no original ACK",false);f.rx(original,1500);const auto first=counts(f);
    original[12]|=8;f.rx(original,2000);unchanged(f,first);f.poll(3000);
    assert(!f.radio.sends&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
    pass("a duplicate cannot add ACK eligibility to an original without wantACK");}
  {Fixture f;assert(f.s.armEndpoint(false));const auto original=f.incoming();f.rx(original,1500);const auto first=counts(f);
    assert(f.s.armEndpoint(true));f.rx(original,2000);unchanged(f,first);f.poll(3000);
    assert(!f.radio.sends&&f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
    pass("an original accepted while disarmed cannot gain a repeat ACK after rearming");}
  {Fixture f;const auto original=f.incoming();f.rx(original,1500);sendAck(f,2000);const auto first=counts(f);
    assert(f.s.armEndpoint(false));f.rx(original,2500);unchanged(f,first);
    assert(f.s.armEndpoint(true));f.rx(original,3000);sendAck(f,3500);
    assert(counts(f).received==first.received&&counts(f).sent==first.sent+1&&f.radio.frames.size()==2);
    pass("a duplicate arriving while disarmed does not consume an already admitted repeat ACK intent");}
  {Fixture f;for(unsigned i=0;i<3;++i){f.rx(f.incoming(90+i),1500+i*1000);sendAck(f,2000+i*1000);}
    const auto first=counts(f);f.rx(f.incoming(90),5000);unchanged(f,first);f.poll(5300);assert(f.radio.frames.size()==3);
    f.rx(f.incoming(91),5500);sendAck(f,6000);f.rx(f.incoming(92),6500);sendAck(f,7000);
    const auto repeated=counts(f);assert(repeated.received==first.received&&repeated.sent==first.sent+2);
    f.rx(f.incoming(91),7500);unchanged(f,repeated);f.poll(8000);
    assert(f.radio.frames.size()==5&&f.delivered(f.usb)==3&&f.delivered(f.ble)==3);
    pass("two-entry FIFO evicts repeat intent only while durable replay IDs still suppress old application delivery");}
  {Fixture f;f.s.measuredUptime(123);const auto original=telemetryRequest(f,90);
    f.rx(original,1500);sendAck(f,2000);const auto first=counts(f);
    uint8_t plain[227]{};size_t n=0;const auto &wire=f.radio.frames[0];
    assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,bench::le32(wire.data()+8),
      wire.data()+16,wire.size()-16,plain,sizeof(plain),n));bench::RouteMetadata m;
    assert(bench::MtCodec::decodeData(plain,n,m)&&m.portnum==67&&m.request_id==90&&!m.wantResponse);
    f.rx(original,2500);unchanged(f,first);f.poll(3000);assert(f.radio.frames.size()==1);
    assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
    pass("duplicate telemetry wantResponse is neither re-synthesized nor substituted with a routing ACK");}
  {Fixture f;auto original=f.incoming();original[12]=uint8_t(1|8|(3<<5));
   f.rx(original,1500);sendAck(f,2000);const auto first=counts(f);const auto writes=f.lm.writes;
   assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
   assert(f.replay.seen(f.peer.binding.node,f.peer.binding.publicKey,90)==ReplayCheck::Seen);
   f.s.stop();f.runtime.freeze();assert(f.identity.close()&&f.ledger.close());
   IdentityJournal identity2;PkiLedger ledger2;
   assert(!identity2.load(f.im,f.deriver)&&!ledger2.load(f.lm));
   PkiLedger::Status st;assert(ledger2.status(st)&&st.received==first.received&&st.sent==first.sent&&st.generation==first.generation);
   LedgerNonceAuthority nonce2{identity2,ledger2,f.lm,f.entropy,f.gate};
   LedgerReplayAuthority replay2{identity2,ledger2,f.lm,f.gate};
   PkiRuntime runtime2{f.crypto,f.backend,identity2,nonce2,replay2};
   assert(runtime2.open(f.local.binding.node,f.peer.binding.node));
   mt::test::KnownChannelFixtureClock knownChannelLocal1;mt::Service s2{f.radio,true,f.local.binding.node,81,true,false,&runtime2};knownChannelLocal1.bind(s2);
   mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);
   assert(s2.apply(cfg)&&s2.start(false,0)&&s2.armEndpoint(true));f.gate.service=&s2;
   mt::companion::CompanionWorkspace workspace2;mt::Companion c2{s2,f.usb,81,workspace2,0},bc2{s2,f.ble,81,workspace2,1};
   const auto tx=f.radio.frames.size();const auto sends=f.radio.sends;
   s2.rx(original.data(),original.size(),3,5000,81);
   mt::pki::PrivateEvent ev;assert(!s2.takePrivateEvent(0,original.data(),original.size(),ev));
   c2.receive(original.data(),original.size(),-80,3,5000,mt::Companion::WorkspaceProof{true});
   bc2.receive(original.data(),original.size(),-80,3,5000,mt::Companion::WorkspaceProof{true});
   s2.poll(5100,71);c2.poll(mt::Companion::WorkspaceProof{true});bc2.poll(mt::Companion::WorkspaceProof{true});
   assert(f.radio.frames.size()==tx&&f.radio.sends==sends&&f.lm.writes==writes);
   assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
   PkiLedger::Status st2;assert(ledger2.status(st2)&&st2.received==st.received&&st2.sent==st.sent&&st2.generation==st.generation);
   assert(replay2.seen(f.peer.binding.node,f.peer.binding.publicKey,90)==ReplayCheck::Seen);
   pass("modeled cold reconstruction retains durable Seen authority but loses RAM repeat intent; duplicate has no ACK, event, or Companion delivery");}
  {Fixture f;auto original=f.incoming();
   f.rx(original,1500);sendAck(f,2000);const auto first=counts(f);
   const auto tx=f.radio.frames.size();const auto sends=f.radio.sends;const auto writes=f.lm.writes;
   f.lm.fault=2;
   f.rx(original,2500);f.poll(3000);
   assert(f.radio.frames.size()==tx&&f.radio.sends==sends);
   assert(f.delivered(f.usb)==1&&f.delivered(f.ble)==1);
   assert(f.lm.writes==writes+1);
   f.lm.fault=0;
   PkiLedger fresh;
   assert(!fresh.load(f.lm));
   PkiLedger::Status recovered;
   assert(fresh.status(recovered));
   assert(recovered.received==first.received&&recovered.sent==first.sent+1&&recovered.generation==first.generation+1);
   assert(fresh.sent(0x412a6a9e));
   assert(fresh.reserve(f.lm,0x412a6a9e,false)==-EALREADY);
   pass("full-write-then-error freezes TX; reload proves ambiguous ID is reserved and cannot be reused");}
{
  Fixture f;
  const auto base=counts(f);
  const uint32_t rxAt[4]={1500,1501,1502,1503},rng[4]={1001,1002,1003,1004};
  for(unsigned i=0;i<4;++i){
    const auto fr=f.incoming(90+i);
    f.s.rx(fr.data(),fr.size(),3,rxAt[i],rng[i]);
    f.c.receive(fr.data(),fr.size(),-80,3,rxAt[i],mt::Companion::WorkspaceProof{true});
    f.bc.receive(fr.data(),fr.size(),-80,3,rxAt[i],mt::Companion::WorkspaceProof{true});
  }
  const auto mid=counts(f);
  assert(mid.received==base.received+4&&mid.sent==base.sent+4&&mid.generation==base.generation+8);
  assert(f.delivered(f.usb)==4&&f.delivered(f.ble)==4);
  const auto dup=f.incoming(93);
  f.s.rx(dup.data(),dup.size(),3,1600,0x7001);
  f.c.receive(dup.data(),dup.size(),-80,3,1600,mt::Companion::WorkspaceProof{true});
  f.bc.receive(dup.data(),dup.size(),-80,3,1600,mt::Companion::WorkspaceProof{true});
  const auto after=counts(f);
  assert(after.sent==mid.sent+1&&after.generation==mid.generation+1&&after.received==mid.received&&
         after.stored==mid.stored&&after.active==mid.active&&after.format==mid.format);
  assert(f.delivered(f.usb)==4&&f.delivered(f.ble)==4);
  assert(2000-1503>=250);
  sendAck(f,2000);
  sendAck(f,2500);
  sendAck(f,3000);
  sendAck(f,3500);
  unchanged(f,after);
  assert(f.radio.frames.size()==4);
  const auto dup2=f.incoming(93);
  f.s.rx(dup2.data(),dup2.size(),3,4000,0x7002);
  f.c.receive(dup2.data(),dup2.size(),-80,3,4000,mt::Companion::WorkspaceProof{true});
  f.bc.receive(dup2.data(),dup2.size(),-80,3,4000,mt::Companion::WorkspaceProof{true});
  const unsigned frames=f.radio.frames.size();
  f.poll(4500);
  assert(f.radio.frames.size()==frames);
  unchanged(f,after);
  assert(f.delivered(f.usb)==4&&f.delivered(f.ble)==4);
  pass("full-ACK-queue-burns-one-repeat-reservation-without-redelivery-or-later-retry");
}
}
