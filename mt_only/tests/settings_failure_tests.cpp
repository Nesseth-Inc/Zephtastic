// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include <cstdlib>
struct Captured {uint8_t model;const char *hex;};
static const Captured captured[]={{81,"94c300330a310dc8b8a14415c8b8a144221e08061213920210320e080138014003480150025814680118013510e198e03510e198e05001"},{127,"94c3003c0a3a0d2794e144152794e14422270806121c9202193217080138014003480150f7ffffffffffffffff0158146801180135e1d572d935e1d572d95001"},{127,"94c3003c0a3a0d2794e144152794e14422270806121c9202193217080138014003480150f7ffffffffffffffff0158146801180135292e39ff35292e39ff5001"}};
static meshtastic_ToRadio request(const Captured &c) {
 std::vector<uint8_t> bytes;for(const char *p=c.hex;*p;p+=2){char b[3]={p[0],p[1],0};bytes.push_back(uint8_t(strtoul(b,nullptr,16)));}
 assert(bytes[0]==0x94&&bytes[1]==0xc3&&bytes.size()==size_t(bytes[2]*256+bytes[3]+4));
 meshtastic_ToRadio r{};auto in=pb_istream_from_buffer(bytes.data()+4,bytes.size()-4);assert(pb_decode(&in,meshtastic_ToRadio_fields,&r));
 // Bind only envelope node addresses to the fixture identity; captured ID and
 // exact admin payload bytes are unchanged. No actual radio is instantiated.
 r.packet.from=Node;r.packet.to=Node;return r;
}
static void provision(Fixture &f) {
 auto cfg=f.service.config;cfg.bytes[4]=1;cfg.setTxEnabled(false);assert(f.edit.save(cfg,0)==0);assert(f.service.start(false,0));assert(!f.service.endpointArmed());
}
int main() {
 for(const auto &c:captured)for(int condition=0;condition<4;++condition) {
  auto f=std::make_unique<Fixture>(c.model,false);provision(*f);auto r=request(c);const auto before=f->service.config;const auto writes=f->backend.writes;
  if(condition==1)f->radio.busyFlag=true;
  if(condition==2)f->radio.pauseResult=-EAGAIN;
  if(condition==3)f->backend.fail=true;
  f->input(r,false,6375);const int error=f->usb.error();
#ifdef EXPECT_REPAIRED
  const int expected=condition==0?0:condition==3?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_RATE_LIMIT_EXCEEDED;
#else
  const int expected=condition==0?0:condition==3?meshtastic_Routing_Error_NO_INTERFACE:meshtastic_Routing_Error_BAD_REQUEST;
#endif
  std::cerr<<"OBS model="<<unsigned(c.model)<<" condition="<<condition<<" error="<<error<<" expected="<<expected<<" saves="<<f->usb.saves<<" writes="<<f->backend.writes<<std::endl;
  assert(error==expected);
  if(condition==0){assert(f->service.config.txEnabled()&&f->service.endpointArmed());assert(f->backend.writes==writes+1);}
  else {assert(f->service.config.same(before)&&!f->service.endpointArmed());if(condition!=3)assert(f->backend.writes==writes&&f->service.receiving()&&!f->service.faulted());}
  assert(f->radio.sends==0);
  if(condition==1||condition==2){f->radio.busyFlag=false;f->radio.pauseResult=0;r.packet.id++;f->input(r,false,70000);assert(f->usb.error()==0&&f->service.endpointArmed()&&f->backend.writes==writes+1);}
  std::cout<<"PASS captured model="<<unsigned(c.model)<<" condition="<<condition<<" error="<<error<<"\n";
 }
 // An invalid region remains a malformed request and never reaches persistence.
 auto f=std::make_unique<Fixture>(81,false);provision(*f);auto r=request(captured[0]);meshtastic_AdminMessage a{};
 auto in=pb_istream_from_buffer(r.packet.decoded.payload.bytes,r.packet.decoded.payload.size);assert(pb_decode(&in,meshtastic_AdminMessage_fields,&a));a.set_config.payload_variant.lora.region=meshtastic_Config_LoRaConfig_RegionCode_EU_868;
 auto out=pb_ostream_from_buffer(r.packet.decoded.payload.bytes,sizeof(r.packet.decoded.payload.bytes));assert(pb_encode(&out,meshtastic_AdminMessage_fields,&a));r.packet.decoded.payload.size=out.bytes_written;
 auto writes=f->backend.writes;f->radio.busyFlag=true;f->input(r);assert(f->usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f->backend.writes==writes);std::cout<<"PASS invalid region retains BAD_REQUEST without writes\n";
}
