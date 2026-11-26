// SPDX-License-Identifier: GPL-3.0-only
// Independent boundary checks. Synthetic frames and mock radio only.
// The existing fixture supplies transport setup, not the test expectations.
#include "conformance_fixture.h"
#include <functional>
#include <string>

static unsigned textCount(const Host &h) {
  unsigned n=0;
  for(const auto &r:h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      r.packet.which_payload_variant==meshtastic_MeshPacket_decoded_tag&&
      r.packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP)++n;
  return n;
}
static void admin(Fixture &f,const meshtastic_AdminMessage &a,uint32_t id=1000) {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.id=id;p.to=OURS;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_ADMIN_APP;
  auto out=pb_ostream_from_buffer(p.decoded.payload.bytes,sizeof(p.decoded.payload.bytes));
  assert(pb_encode(&out,meshtastic_AdminMessage_fields,&a));p.decoded.payload.size=out.bytes_written;
  input(f.c,r,100);
}
static std::vector<uint8_t> rawText(uint32_t source,uint32_t id) {
  // Encode bytes independently, including invalid source values that the
  // outgoing encoder properly refuses. Data(port=1, payload="x", bitfield=0).
  const uint8_t data[]={8,1,18,1,'x',72,0};
  std::vector<uint8_t> frame(16+sizeof(data));
  auto put=[&](unsigned at,uint32_t v){for(unsigned i=0;i<4;++i)frame[at+i]=v>>(8*i);};
  put(0,OURS);put(4,source);put(8,id);frame[13]=bench::MtNodeRecord::publicHash();
  memcpy(frame.data()+16,data,sizeof(data));uint8_t nonce[16];bench::MtCodec::nonce(source,id,nonce);
  assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,frame.data()+16,sizeof(data)));
  return frame;
}
static bool reserved_source_filter() {
  auto f=std::make_unique<Fixture>();
  for(uint32_t source:{0u,1u,0xfffffffeu,0xffffffffu})f->rx(rawText(source,source+17));
  f->s.poll(1000,0);
  return textCount(f->h)==0&&textCount(f->bh)==0&&f->radio.sends==0;
}
static bool nak_reason_preserved() {
  // Public NAKs are unauthenticated. This checks accurate error reporting,
  // not authentication; wrong-peer/unsolicited NAKs remain rejected below.
  for(unsigned code:{1u,6u,7u,9u,32u,35u,38u}) {
    auto f=std::make_unique<Fixture>();f->text(1);f->s.poll(300,0);f->done();
    f->rx(f->wire(PEER,OURS,100,1,code));f->clients();
    if(results(f->h,1,code)!=1||f->s.endpointFree()!=4)return false;
    f->s.poll(50000,0);if(f->radio.sends!=1)return false;
  }
  return true;
}
static bool nak_scope_checked() {
  auto f=std::make_unique<Fixture>();f->text(1);
  f->rx(f->wire(PEER,OURS,80,1,6),200);f->s.poll(300,0);f->done();
  f->rx(f->wire(PEER+1,OURS,81,1,6));
  f->rx(f->wire(PEER,OURS+1,82,1,6));
  f->rx(f->wire(PEER,OURS,83,2,6));f->clients();
  return !results(f->h,1)&&f->s.endpointFree()==3&&f->radio.sends==1;
}
static bool unsupported_rx_gain_not_silent_success() {
  auto f=std::make_unique<Fixture>();meshtastic_AdminMessage a{};
  a.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  a.set_config.which_payload_variant=meshtastic_Config_lora_tag;
  auto &l=a.set_config.payload_variant.lora;
  l.region=meshtastic_Config_LoRaConfig_RegionCode_US;l.use_preset=true;l.hop_limit=3;
  l.channel_num=20;l.tx_power=2;l.sx126x_rx_boosted_gain=false;
  admin(*f,a,1000);
  if(results(f->h,1000,32)==1)return true; // Explicit unsupported-field refusal.
  if(results(f->h,1000,0)!=1)return false;
  f->h.out.clear();a={};a.which_payload_variant=meshtastic_AdminMessage_get_config_request_tag;
  a.get_config_request=meshtastic_AdminMessage_ConfigType_LORA_CONFIG;admin(*f,a,1001);
  for(const auto &r:f->h.out)if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&
      r.packet.decoded.portnum==meshtastic_PortNum_ADMIN_APP) {
    meshtastic_AdminMessage answer{};auto in=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);
    assert(pb_decode(&in,meshtastic_AdminMessage_fields,&answer));
    if(answer.which_payload_variant==meshtastic_AdminMessage_get_config_response_tag)
      return !answer.get_config_response.payload_variant.lora.sx126x_rx_boosted_gain;
  }
  return false;
}
static bool user_signature_not_silently_stripped() {
  auto f=std::make_unique<Fixture>();meshtastic_ToRadio r{};
  r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.id=1000;p.to=PEER;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=1;p.decoded.payload.bytes[0]='x';
  p.decoded.xeddsa_signature.size=64;memset(p.decoded.xeddsa_signature.bytes,0x5a,64);
  input(f->c,r,100);f->s.poll(300,0);
  return results(f->h,1000,32)==1&&f->radio.sends==0&&f->s.endpointFree()==4;
}
static bool corrupt_config_rejected() {
  mt::Config original;
  for(unsigned byte=0;byte<sizeof(original.bytes);++byte)for(unsigned bit=0;bit<8;++bit) {
    auto candidate=original;candidate.bytes[byte]^=1u<<bit;if(candidate.valid())return false;
  }
  for(unsigned byte:{0u,1u,2u,3u,4u,5u}) {
    auto candidate=original;candidate.bytes[byte]=255;candidate.seal();if(candidate.valid())return false;
  }
  auto f=std::make_unique<Fixture>();auto invalid=f->s.config;invalid.bytes[4]=2;invalid.seal();
  f->s.stop();return !f->s.apply(invalid)&&!f->s.receiving()&&!f->s.endpointArmed();
}
static bool oversize_text_refused_without_queue_use() {
  auto f=std::make_unique<Fixture>();meshtastic_ToRadio r{};
  r.which_payload_variant=meshtastic_ToRadio_packet_tag;auto &p=r.packet;
  p.id=1000;p.to=PEER;p.hop_limit=3;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;
  p.decoded.payload.size=sizeof(p.decoded.payload.bytes);memset(p.decoded.payload.bytes,'x',p.decoded.payload.size);
  input(f->c,r,100);f->s.poll(300,0);
  return results(f->h,1000,7)==1&&f->s.endpointFree()==4&&!f->radio.sends;
}
static bool serial_timeout_resynchronizes() {
  auto f=std::make_unique<Fixture>();const auto before=f->c.handshakes;
  // Abandon a declared 512-byte frame, then overlap sync bytes after timeout.
  for(uint8_t b:{0x94,0xc3,0x02,0x00,0x08})f->c.byte(b,100,0,mt::Companion::WorkspaceProof{true});
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=222;
  uint8_t packet[512];auto out=pb_ostream_from_buffer(packet,sizeof(packet));
  assert(pb_encode(&out,meshtastic_ToRadio_fields,&r));
  f->c.byte(0x94,2201,0,mt::Companion::WorkspaceProof{true});f->c.byte(0x94,2201,0,mt::Companion::WorkspaceProof{true});f->c.byte(0xc3,2201,0,mt::Companion::WorkspaceProof{true});
  f->c.byte(out.bytes_written>>8,2201,0,mt::Companion::WorkspaceProof{true});f->c.byte(out.bytes_written,2201,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<out.bytes_written;++i)f->c.byte(packet[i],2201,0,mt::Companion::WorkspaceProof{true});
  return f->c.handshakes==before+1&&f->radio.sends==0;
}
static bool late_nak_does_not_replace_timeout() {
  auto f=std::make_unique<Fixture>();f->text(1);f->s.poll(300,0);f->done();
  f->rx(f->wire(PEER,OURS,100,1,6),90100);f->clients();
  return results(f->h,1,3)==1&&results(f->h,1)==1&&f->s.endpointFree()==4;
}
static bool authenticated_label_not_invented() {
  auto f=std::make_unique<Fixture>();f->text(1);f->s.poll(300,0);f->done();
  f->rx(f->wire(PEER,OURS,100,1));f->clients();
  bool labelled=false,correlated=false;
  for(const auto &r:f->h.out){
    if(r.which_payload_variant==meshtastic_FromRadio_clientNotification_tag&&
       strstr(r.clientNotification.message,"Public-channel peer ACK matched; unauthenticated"))labelled=true;
    if(r.which_payload_variant==meshtastic_FromRadio_packet_tag&&r.packet.decoded.portnum==meshtastic_PortNum_ROUTING_APP&&r.packet.decoded.request_id==1){
      assert(r.packet.ack_proof_status==meshtastic_MeshPacket_AckProofStatus_ACK_PROOF_ABSENT&&!r.packet.pki_encrypted);
      correlated=true;
    }
  }
  return labelled&&correlated;
}
static bool control_echo_stops_retry() {
  auto f=std::make_unique<Fixture>();
  f->rx(f->wire(PEER,OURS,101,0,0,(3<<5)|3|8,true),1000,700);
  f->s.poll(1300,0);if(f->radio.sends!=1)return false;f->done(1301);
  auto echo=f->radio.frames[0];if(!(echo[12]&8))return false;
  echo[12]=uint8_t((echo[12]&~7u)|((echo[12]&7u)-1));echo[15]=0x55;
  f->rx(echo,2000);
  // First poll only schedules a retry. The second exposes a dropped echo.
  f->s.poll(20000,0);f->s.poll(21000,0);
  return f->radio.sends==1;
}
static bool changed_control_echo_does_not_stop_retry() {
  auto f=std::make_unique<Fixture>();
  f->rx(f->wire(PEER,OURS,101,0,0,(3<<5)|3|8,true),1000,700);
  f->s.poll(1300,0);if(f->radio.sends!=1)return false;f->done(1301);
  auto echo=f->radio.frames[0];if(!(echo[12]&8))return false;
  echo[12]=uint8_t((echo[12]&~7u)|((echo[12]&7u)-1));echo[15]=0x55;
  echo.back()^=1;f->rx(echo,2000);
  f->s.poll(20000,0);f->s.poll(21000,0);
  return f->radio.sends==2&&f->radio.frames[0]==f->radio.frames[1];
}
static bool terminal_control_ack_no_loop() {
  auto f=std::make_unique<Fixture>();
  f->rx(f->wire(PEER,OURS,101,0,0,8,true),1000,700);
  f->s.poll(1300,0);if(f->radio.sends!=1)return false;f->done(1301);
  const auto &ack=f->radio.frames[0];if(!(ack[12]&8))return false;
  const auto id=bench::le32(ack.data()+8);
  f->rx(f->wire(PEER,OURS,102,id,0,8),2000);
  f->s.poll(2400,0);f->s.poll(20000,0);f->s.poll(21000,0);
  return f->radio.sends==1&&results(f->h,id)==0;
}
int main(int argc,char **argv) {
  struct Test {const char *name;std::function<bool()> run;};
  const Test tests[]={
    {"reserved_source_filter",reserved_source_filter},
    {"nak_reason_preserved",nak_reason_preserved},
    {"nak_scope_checked",nak_scope_checked},
    {"unsupported_rx_gain_not_silent_success",unsupported_rx_gain_not_silent_success},
    {"user_signature_not_silently_stripped",user_signature_not_silently_stripped},
    {"corrupt_config_rejected",corrupt_config_rejected},
    {"oversize_text_refused_without_queue_use",oversize_text_refused_without_queue_use},
    {"serial_timeout_resynchronizes",serial_timeout_resynchronizes},
    {"late_nak_does_not_replace_timeout",late_nak_does_not_replace_timeout},
    {"authenticated_label_not_invented",authenticated_label_not_invented},
    {"control_echo_stops_retry",control_echo_stops_retry},
    {"changed_control_echo_does_not_stop_retry",changed_control_echo_does_not_stop_retry},
    {"terminal_control_ack_no_loop",terminal_control_ack_no_loop}
  };
  unsigned failed=0,executed=0;
  for(const auto &test:tests)if(argc==1||test.name==std::string(argv[1])) {
    ++executed;const bool ok=test.run();failed+=!ok;
    std::cout<<(ok?"PASS ":"FAIL ")<<test.name<<"\n";
  }
  return !executed||failed?1:0;
}
