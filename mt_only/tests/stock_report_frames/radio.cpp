// SPDX-License-Identifier: GPL-3.0-only
#include "../endpoint_fixture.h"
#include "ui/ReportFrames.h"
struct DisplayHost:Host {
  mt::ui::ReportFrames history;
  unsigned observed=0,withdrawn=0;uint64_t clock=0;
  using Host::Host;
  void displayReport(uint32_t from,uint32_t id,bool shared,const meshtastic_Telemetry &report)override{
    assert(from==PEER);
    auto lookup=[](uint32_t node){return mt::ui::ReportFrames::Origin{node,3,"RF"};};
    history.validate(OURS,1,clock,lookup);
    history.observe(lookup(from),shared,id,clock,report);
    if(shared)++observed;else ++withdrawn;
  }
};
int main(){
  Fixture f;DisplayHost host(f.s);mt::companion::CompanionWorkspace workspace;
  mt::Companion companion(f.s,host,81,workspace);
  auto deliver=[&](uint32_t id,uint32_t to,float temperature,bool want=false,bool malformed=false){
    meshtastic_Telemetry t{};t.which_variant=meshtastic_Telemetry_environment_metrics_tag;
    auto &e=t.variant.environment_metrics;e.has_temperature=true;e.temperature=temperature;
    meshtastic_Data d{};d.portnum=meshtastic_PortNum_TELEMETRY_APP;d.has_bitfield=true;d.want_response=want;
    auto payload=pb_ostream_from_buffer(d.payload.bytes,sizeof(d.payload.bytes));assert(pb_encode(&payload,meshtastic_Telemetry_fields,&t));
    d.payload.size=payload.bytes_written;if(malformed)d.payload.bytes[d.payload.size++]=0;
    uint8_t bytes[239],frame[255];auto out=pb_ostream_from_buffer(bytes,sizeof(bytes));assert(pb_encode(&out,meshtastic_Data_fields,&d));
    const size_t n=16+out.bytes_written;
    memset(frame,0,16);for(unsigned i=0;i<4;++i){
      frame[i]=uint8_t(to>>(8*i));frame[4+i]=uint8_t(PEER>>(8*i));frame[8+i]=uint8_t(id>>(8*i));
    }
    frame[13]=8;frame[15]=uint8_t(PEER);memcpy(frame+16,bytes,out.bytes_written);
    uint8_t nonce[16];bench::MtCodec::nonce(PEER,id,nonce);
    assert(bench::mtAesCtr(nullptr,bench::MtNodeRecord::PublicKey,16,nonce,frame+16,out.bytes_written));
    f.s.rx(frame,n,3,++host.clock,700);companion.receive(frame,n,-90,3,host.clock,mt::Companion::WorkspaceProof{true});
  };
  deliver(101,UINT32_MAX,12.5f);assert(host.observed==1 && strstr(host.history.page(0,1,false).rows[0],"12.5C"));
  deliver(101,UINT32_MAX,18);assert(host.observed==1);
  deliver(102,UINT32_MAX,19,true);deliver(103,UINT32_MAX,20,false,true);deliver(104,UINT32_MAX,NAN);
  assert(host.observed==1 && strstr(host.history.page(0,1,false).rows[0],"12.5C"));
  deliver(105,OURS,25);assert(host.withdrawn==1 && strstr(host.history.page(0,1,false).source,"No measurement"));
  deliver(106,UINT32_MAX,30);assert(host.observed==2 && strstr(host.history.page(0,1,false).rows[0],"30.0C"));
  deliver(107,0x31415926,40);assert(host.observed==2 && host.withdrawn==1);
  assert(!results(host,999));
  group("actual Companion RF receive and UI projection without a phone session, broadcast, dedup, malformed, NaN, directed withdrawal and other destination");
}
