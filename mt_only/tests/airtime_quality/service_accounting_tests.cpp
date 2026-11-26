// SPDX-License-Identifier: GPL-3.0-only
// Independent acceptance source. Adapter must bind the delivered public API.
// No hardware/RF. Do not report a PASS until compiled against the frozen patch.
#include "endpoint_fixture.h"
#include "known_channel_fixture.h"
#include <cmath>
#include <string>
#include "observation_adapter.h"
#include "../../dependency-overlay/zephyr/drivers/lora/native/sx126x/sx126x_airtime_math.h"
#ifdef NDEBUG
#error Acceptance requires enabled assertions
#endif
using qualify::Clock;
struct SizedRadio:Radio {
 int fixed=100;
 uint32_t airtime(size_t n)override{return fixed>=0?uint32_t(fixed):uint32_t(n);}
};
struct Harness {
 Clock clock;mt::test::KnownChannelFixtureClock channelClock;SizedRadio radio;mt::Service s;Host host;mt::companion::CompanionWorkspace workspace;mt::Companion client;
 explicit Harness(bool bound=true,uint64_t initial=0,bool normal=true,bool relay=false):clock{initial,true},s(radio,true,OURS,81,normal,false),host(s),client(s,host,81,workspace,0) {
  channelClock.bind(s);
  if(bound)qualify::bindObservation(s,clock);
  mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
  assert(s.apply(cfg)&&s.start(relay,uint32_t(initial))&&s.armEndpoint(true));
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;r.want_config_id=1;
  input(client,r,uint32_t(initial)+1);host.out.clear();
 }
 void text(uint32_t id=1,uint32_t now=100,const std::string &message="Hello") {
  meshtastic_ToRadio r{};r.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &p=r.packet;p.id=id;p.to=PEER;p.hop_limit=0;p.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  p.decoded.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;p.decoded.payload.size=message.size();
  assert(message.size()<=sizeof(p.decoded.payload.bytes));memcpy(p.decoded.payload.bytes,message.data(),message.size());
  input(client,r,now);
 }
 void complete(uint32_t now=301){radio.done=true;s.poll(now,0);}
 bool value(float &out){return qualify::observedPercent(s,out);}
 void expected(uint64_t durationMs) {
  float out=-1;assert(value(out));assert(std::isfinite(out)&&out>=0&&out<=100);
  const double want=double(durationMs)/36000.0;
  assert(std::fabs(double(out)-want)<0.00000003);
 }
 void unavailable(){float out=-1;assert(!value(out));}
 void notCounted(){float out=-1;if(value(out))assert(out==0);}
};
static std::vector<uint8_t> publicFrame(mt::Service &service,uint32_t from,uint32_t to,uint32_t id,bool trace=false) {
 uint8_t data[239]{},frame[255]{};size_t size=0,bytes=0;
 if(trace)assert(mt::TraceRoute::requestData(nullptr,0,data,sizeof(data),size));
 else {
  meshtastic_Data d{};d.portnum=meshtastic_PortNum_TEXT_MESSAGE_APP;d.has_bitfield=true;
  d.payload.size=5;memcpy(d.payload.bytes,"probe",5);auto out=pb_ostream_from_buffer(data,sizeof(data));
  assert(pb_encode(&out,meshtastic_Data_fields,&d));size=out.bytes_written;
 }
 assert(trace?service.codec.encodeTraceRouteRequest(from,to,id,data,size,frame,sizeof(frame),bytes):
              service.codec.encodeZeroHopData(from,to,id,data,size,frame,sizeof(frame),bytes));
 return {frame,frame+bytes};
}
int main(){
 (void)&results;
 {Harness f(false);f.unavailable();assert(!f.radio.sends);}
 {Harness f;f.expected(0);assert(!f.radio.sends);}
 group("unknown provider differs from known idle zero before any TX");
 for(int air:{1,600,2000}) {
  Harness f;f.radio.fixed=air;f.text();f.s.poll(300,0);assert(f.radio.sends==1);f.notCounted();
  f.radio.fixed=2499;f.complete();f.expected(air);assert(f.s.sent==1);
  f.s.poll(302,0);f.s.poll(303,0);f.expected(air);assert(f.radio.sends==1&&f.s.sent==1);
 }
 group("fractional accepted estimate cached at start; completion once despite estimate changes and sticky poll");
 {Harness f;f.radio.fixed=2000;
  for(unsigned i=0;i<3;++i){const uint32_t at=100+i*1000;f.text(10+i,at);f.s.poll(at+200,0);f.complete(at+201);}
  assert(f.radio.sends==3);f.expected(6000);
  f.clock.ms=3599999;f.expected(6000);f.clock.ms=3600000;f.expected(0);assert(f.radio.sends==3);
 }
 group("6000ms gives0.166667percent independently of600000ms admission budget; idle read expires hour bucket");
 {Harness f;f.radio.fixed=-1;f.text(7,100,"different frame size");f.s.poll(300,0);
  assert(f.radio.frames.size()==1);const size_t actual=f.radio.frames[0].size();
  f.radio.fixed=2400;f.complete();f.expected(actual);
 }
 group("length-aware radio prevents cached job or later estimate substitution");
 for(bool restoreFails:{false,true}) {Harness f;f.radio.fixed=600;f.text();f.s.poll(300,0);
  if(restoreFails)f.radio.restoreStatus=-EIO;else f.radio.healthStatus=0;
  f.complete();assert(f.s.faulted()&&f.s.sent==0);f.expected(600);assert(f.radio.sends==1);
 }
 group("known completion is credited before failed RX restoration without changing original fault/sent behavior");
 for(bool busy:{false,true}) {Harness f;f.radio.sendStatus=busy?-EBUSY:-EIO;f.text();f.s.poll(300,0);
  assert(f.radio.sends==1);float out=-1;if(f.value(out))assert(out==0);assert(f.s.sent==0);
  assert(f.s.faulted()==!busy&&f.s.receiving()==busy);}
 group("normal endpoint admits long manual airtime, keeps active through computed deadline, then credits known completion once");
 {Harness f;auto cfg=f.s.config;assert(cfg.setPrimaryName("LongFast"));auto request=cfg.radioRequest();
  request.usePreset=false;request.bandwidth=8;request.spreadingFactor=12;request.codingRate=8;
  assert(cfg.setRadio(request));f.s.stop();assert(f.s.apply(cfg)&&f.s.start(false,1)&&f.s.armEndpoint(true));
  const sx126x_airtime_input modem{7810,16,12,4,true,true};const auto minimum=sx126x_airtime_estimate(&modem,0);
  assert(minimum.trustworthy&&minimum.ms>2500);f.radio.fixed=int(minimum.ms);
  mt::RadioTiming::Deadline timing;assert(mt::RadioTiming::resolve(minimum.ms,timing));
  const auto free=f.s.endpointFree();f.text(700,100);assert(f.s.endpointFree()+1==free);
  f.s.poll(300,0);assert(f.radio.sends==1&&f.s.active()&&!f.s.sent&&!f.s.faulted());
  f.s.poll(300+timing.hostMs-1,0);mt::Endpoint::Result result;
  assert(f.s.active()&&!f.s.takeMessageResult(0,result)&&f.radio.sends==1&&!f.s.faulted());f.notCounted();
  f.clock.ms=1000;f.complete(300+timing.hostMs);
  assert(f.s.takeMessageResult(0,result)&&result.id==700&&result.code==0);
  assert(!f.s.active()&&f.s.sent==1&&!f.s.faulted()&&f.s.receiving()&&f.radio.sends==1);f.expected(minimum.ms);
  f.s.poll(301+timing.hostMs,0);f.expected(minimum.ms);assert(f.radio.sends==1&&f.s.sent==1);}
 group("uncertain completion remains uncredited and unavailable; recovery does not replay TX");
 for(unsigned mode=0;mode<4;++mode) {Harness f;f.radio.fixed=600;f.text();f.s.poll(300,0);assert(f.radio.sends==1);
  mt::RadioTiming::Deadline timeout;assert(mt::RadioTiming::resolve(600,timeout));
  f.clock.ms=1000;
  if(mode==0)f.s.poll(300+timeout.hostMs,0);
  if(mode==1)f.s.stop();
  if(mode==2)assert(f.s.armEndpoint(false));
  if(mode==3){f.radio.doneStatus=-EIO;f.complete();}
  f.unavailable();assert(f.s.sent==0);assert(f.radio.sends==1);
  for(uint64_t now:{1001ull,59999ull,60000ull,1800000ull,3599999ull,3600999ull}){f.clock.ms=now;f.unavailable();assert(f.radio.sends==1);}
  f.clock.ms=3601000;f.expected(0);assert(f.radio.sends==1);
 }
 group("timeout stop disarm and failed completion stay unavailable through conservative hour; recovery adds no TX");
 {Harness f(true,0xfffffe00ull);f.radio.fixed=1;f.text(1,0xfffffe00u);
  f.clock.ms=0xffffff00ull;f.s.poll(0xffffff00u,0);assert(f.radio.sends==1);
  f.clock.ms=0x100000010ull;f.complete(0x10u);f.expected(1);assert(f.radio.sends==1);
 }
 group("true64-bit observation clock crosses49.7day scheduling boundary without resetting accounting");
 {Harness f(true,58000);f.radio.fixed=600;f.clock.ms=58200;f.text(1,58200);
  f.clock.ms=58400;f.s.poll(58400,0);assert(f.radio.sends==1);
  f.clock.ms=59999;f.complete(59999);f.expected(600);
  f.clock.ms=3599999;f.expected(600);f.clock.ms=3600000;f.expected(0);assert(f.radio.sends==1);
 }
 group("near-minute completion expires at60th absolute bucket rather than3600000ms after sample");
 {Harness f;f.radio.fixed=600;f.text();f.s.poll(300,0);f.clock.ms=1000;f.complete();f.expected(600);
  f.clock.ms=999;f.unavailable();assert(f.radio.sends==1);}
 group("backward observation clock cannot fabricate a zero or percentage");
 {Harness f(true,1000);f.expected(0);f.clock.ms=999;f.unavailable();assert(!f.radio.sends);}
 group("backward clock before first completed sample cannot fabricate idle zero");
 {Harness f;f.radio.fixed=600;f.text();f.s.poll(300,0);assert(f.radio.sends==1);
  f.clock.valid=false;f.complete();f.unavailable();f.clock.valid=true;
  for(uint64_t now:{1000ull,60000ull,3599999ull,3600999ull}){f.clock.ms=now;f.unavailable();}
  f.clock.ms=3601000;f.expected(0);assert(f.radio.sends==1);}
 group("lost clock at credit retains unknown quality until a full hour from first trustworthy recovery observation");
 {Harness f(false);f.radio.fixed=600;f.text();f.s.poll(300,0);assert(f.radio.sends==1);f.complete();
  f.clock.ms=1000;qualify::bindObservation(f.s,f.clock);
  for(uint64_t now:{1000ull,60000ull,3599999ull,3600999ull}){f.clock.ms=now;f.unavailable();}
  // A safe API may refuse a late first binding permanently. If it accepts,
  // quality must recover without inventing a timestamp for the lost sample.
  f.clock.ms=3601000;float out=-1;if(f.value(out))assert(out==0);assert(f.radio.sends==1);}
 group("pre-binding TX history cannot be forgotten into a new known-zero observation");
 {Harness f;f.radio.fixed=600;f.text();f.s.poll(300,0);f.clock.ms=1000;f.complete();f.expected(600);
  f.text(2,1000);f.clock.ms=2000;f.s.poll(1300,0);assert(f.radio.sends==2);
  Clock replacement;qualify::bindObservation(f.s,replacement);float value=-1;
  if(f.value(value))assert(std::fabs(double(value)-600.0/36000.0)<0.00000003);
  f.clock.ms=2200;f.complete(1301);
  if(f.value(value))assert(std::fabs(double(value)-1200.0/36000.0)<0.00000003);
  assert(f.radio.sends==2);}
 group("active rebind is either safely refused with old positive history or accepted as unavailable, never erased into zero");
 {Harness muted;auto frame=publicFrame(muted.s,PEER,0xabcdef01,399,true);
  frame[12]=uint8_t((3<<5)|3);frame[15]=uint8_t(PEER);
  muted.clock.ms=100;muted.s.rx(frame.data(),frame.size(),-5,100,0);
  muted.clock.ms=400;muted.s.poll(400,0);
  assert(muted.s.config.role()==meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE&&!muted.s.relaying());
  assert(!muted.radio.sends&&!muted.s.active());muted.expected(0);}
 group("CLIENT_MUTE suppresses normal transit and keeps completed TX airtime at idle zero");
 {Harness f;f.s.stop();auto cfg=f.s.config;
  assert(cfg.setDeviceRole(meshtastic_Config_DeviceConfig_Role_ROUTER,0));
  assert(f.s.apply(cfg)&&f.s.start(false,0)&&f.s.armEndpoint(true)&&f.s.relaying());
  f.radio.fixed=-1;auto frame=publicFrame(f.s,PEER,0xabcdef01,400,true);
  frame[12]=uint8_t((3<<5)|3);frame[15]=uint8_t(PEER);
  f.clock.ms=100;f.s.rx(frame.data(),frame.size(),-5,100,0);
  f.clock.ms=400;f.s.poll(400,0);assert(f.radio.sends==1&&f.s.active());
  const size_t actual=f.radio.frames[0].size();assert(actual>frame.size());
  f.radio.fixed=2400;f.clock.ms=401;f.complete(401);f.expected(actual);
  f.s.poll(402,0);f.expected(actual);assert(f.radio.sends==1&&f.s.sent==1);}
 group("normal traceroute transit caches the rewritten active frame estimate and credits once");
 for(bool restoreFails:{false,true}) {Harness f(true,0,false,false);f.radio.fixed=600;
  auto frame=publicFrame(f.s,OURS,PEER,401);assert(!f.s.submitEndpoint(0,401,frame.data(),frame.size(),100));
  f.clock.ms=300;f.s.poll(300,0);assert(f.radio.sends==1&&f.s.active());f.notCounted();
  f.radio.fixed=2400;if(restoreFails)f.radio.restoreStatus=-EIO;
  f.clock.ms=301;f.complete(301);f.expected(600);assert(f.s.sent==1&&f.s.faulted()==restoreFails);
  f.s.poll(302,0);f.expected(600);assert(f.radio.sends==1);}
 group("legacy one-packet endpoint preserves cached completion credit and original restore-fault sent semantics");
 {Harness f(true,0,false,true);f.radio.fixed=-1;auto frame=publicFrame(f.s,PEER,0xffffffffu,402);
  frame[12]=uint8_t((3<<5)|3);frame[15]=uint8_t(PEER);
  f.clock.ms=100;f.s.rx(frame.data(),frame.size(),0,100,0);f.clock.ms=10000;f.s.poll(10000,0);
  assert(f.radio.sends==1&&f.s.active());const size_t actual=f.radio.frames[0].size();f.radio.fixed=2400;
  f.clock.ms=10001;f.complete(10001);f.expected(actual);f.s.poll(10002,0);f.expected(actual);assert(f.radio.sends==1&&f.s.sent==1);}
 group("legacy relay credits its stable active frame estimate once without changing relay traffic");
 {Harness f(true,0,false,true);f.radio.fixed=600;assert(f.s.announce(1,55));f.clock.ms=300;f.s.poll(300,0);
  assert(f.radio.sends==1&&f.s.active());f.radio.fixed=2400;f.clock.ms=301;f.complete(301);f.expected(600);
  assert(f.s.sent==1&&f.s.discovery.completed==1);f.s.poll(302,0);f.expected(600);assert(f.radio.sends==1);}
 group("explicit legacy discovery completion shares accounting without creating another announcement");
}
