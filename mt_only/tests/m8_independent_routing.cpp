// SPDX-License-Identifier: GPL-3.0-only
// Independent externally observable endpoint checks. The oracle is the pinned
// ReliableRouter::sniffReceived contract, narrowed to ZT's admitted text subset.
// No production internals, physical radio, retry clocks, or device I/O are used.
#include "conformance_fixture.h"

static unsigned runs=0,failed=0;
static void check(bool ok,const char *name) {
  ++runs;failed+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<"\n";
}
static unsigned delivered(const Host &h) {
  unsigned n=0;for(const auto &p:h.out)if(p.which_payload_variant==meshtastic_FromRadio_packet_tag&&
    p.packet.decoded.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP)++n;
  return n;
}
static bool isTerminalAck(Fixture &f,uint32_t request) {
  if(f.radio.frames.size()!=1)return false;
  const auto &p=f.radio.frames[0];bench::RouteMetadata m;
  return bench::le32(p.data())==PEER&&bench::le32(p.data()+4)==OURS&&p[12]==0&&
    f.s.codec.decode(p.data(),p.size(),m)&&m.portnum==5&&m.routingPresent&&
    m.routingErrorPresent&&m.routingError==0&&m.request_id==request;
}
static void nextHopMatrix() {
  unsigned errors=0,vectors=0;
  for(unsigned start=0;start<8;++start)for(unsigned hop=0;hop<=start;++hop)
    for(uint8_t next:{uint8_t(0),uint8_t(OURS),uint8_t(OURS+1)})for(bool armed:{false,true}) {
      auto f=std::make_unique<Fixture>(armed);auto p=f->wire(PEER,OURS,101,0,0,(start<<5)|hop,true);
      p[14]=next;f->rx(p,1000);f->s.poll(1300,0);++vectors;
      const bool expected=armed&&hop>0&&next==uint8_t(OURS);
      if(expected?!isTerminalAck(*f,101):f->radio.sends!=0)++errors;
      if(delivered(f->h)!=1||delivered(f->bh)!=1)++errors;
    }
  std::cout<<"DETAIL next-hop vectors="<<vectors<<" errors="<<errors<<"\n";
  check(!errors,"next_hop_text_ack_header_arm_matrix");
}
static void completedAckMatrix() {
  unsigned errors=0,vectors=0;
  for(unsigned start=0;start<8;++start)for(unsigned hop=0;hop<=start;++hop)
    for(uint8_t next:{uint8_t(0),uint8_t(OURS),uint8_t(OURS+1)})for(unsigned code:{0u,6u}) {
      auto f=std::make_unique<Fixture>();f->text(1);f->s.poll(300,0);f->done(400);
      auto p=f->wire(PEER,OURS,101,1,code,(start<<5)|hop|8);p[14]=next;
      f->rx(p,1000);f->s.poll(1300,0);f->clients();++vectors;
      const bool expected=code==0&&(hop==start||next!=0);
      if(f->radio.sends!=(expected?2:1)||results(f->h,1,code)!=1)++errors;
      if(expected) {
        const auto &ack=f->radio.frames.back();bench::RouteMetadata m;
        if(ack[12]!=0||!f->s.codec.decode(ack.data(),ack.size(),m)||m.request_id!=101||m.routingError)++errors;
      }
    }
  std::cout<<"DETAIL response vectors="<<vectors<<" errors="<<errors<<"\n";
  check(!errors,"terminal_response_ack_direct_next_hop_flood_matrix");
}
static void wrongCorrelation() {
  unsigned errors=0;
  for(unsigned variant=0;variant<4;++variant) {
    auto f=std::make_unique<Fixture>();f->text(1);f->s.poll(300,0);f->done(400);
    auto p=f->wire(variant==0?PEER+1:PEER,variant==1?OURS+1:OURS,101,variant==2?2:1,0,3|(3<<5)|8);
    if(variant==3)p[16]^=8;
    f->rx(p,1000);f->s.poll(1300,0);f->clients();
    if(f->radio.sends!=1||results(f->h,1)||f->s.endpointFree()!=3)++errors;
  }
  check(!errors,"unsolicited_or_changed_ack_neither_completes_nor_triggers_ack");
}
static void repeatedNextHop() {
  auto f=std::make_unique<Fixture>();auto p=f->wire(PEER,OURS,101,0,0,3|(3<<5),true);p[14]=uint8_t(OURS);
  for(uint32_t now:{1000u,4000u,7000u,10000u,13000u}) {
    f->rx(p,now,now);f->s.poll(now+300,0);f->done(now+301);
  }
  check(f->radio.sends==3&&delivered(f->h)==1&&delivered(f->bh)==1,"duplicates_reack_bounded_without_duplicate_client_delivery");
  bool terminal=true;for(const auto &frame:f->radio.frames)terminal&=frame[12]==0;
  check(terminal,"completion_acks_never_request_ack_or_relay");
}
int main() {
  nextHopMatrix();completedAckMatrix();wrongCorrelation();repeatedNextHop();
  std::cout<<"DETAIL groups="<<runs<<" failed="<<failed<<"\n";return failed?1:0;
}
