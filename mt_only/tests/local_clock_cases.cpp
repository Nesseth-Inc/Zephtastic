// SPDX-License-Identifier: GPL-3.0-only
#include "device_config_fixture.h"
#include "clock/LocalTime.h"
#include <ctime>

using mt::clock::LocalTime;
static uint32_t donor(const char *zone,uint32_t utc){
  assert(setenv("TZ",zone,1)==0);tzset();
  time_t now=utc;auto tm=*gmtime(&now);tm.tm_isdst=-1;
  return uint32_t(int64_t(now)+int64_t(difftime(now,mktime(&tm))));
}
static void localLog(const mt::Config &config,Fixture &f,uint32_t utc,uint32_t expected){
  struct Sink:Host {using Host::Host;bool sendIfQueueEmpty(const uint8_t *p,size_t n)override{return send(p,n);}};
  mt::WallClock clock;
  if(utc)assert(clock.set(utc,0)==0);
  Sink sink(f.edit,f.journal);sink.clock=&clock;
  mt::Companion client(f.service,sink,81,f.workspace);
  meshtastic_ToRadio message{};message.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;message.want_config_id=0x12345678;
  uint8_t frame[64];auto stream=pb_ostream_from_buffer(frame,sizeof(frame));assert(pb_encode(&stream,meshtastic_ToRadio_fields,&message));
  client.byte(0x94,1,0,mt::Companion::WorkspaceProof{true});client.byte(0xc3,1,0,mt::Companion::WorkspaceProof{true});client.byte(0,1,0,mt::Companion::WorkspaceProof{true});client.byte(stream.bytes_written,1,0,mt::Companion::WorkspaceProof{true});
  for(size_t i=0;i<stream.bytes_written;++i)client.byte(frame[i],1,0,mt::Companion::WorkspaceProof{true});
  for(unsigned i=0;i<128;++i)client.poll(mt::Companion::WorkspaceProof{true});
  assert(!sink.out.empty()&&sink.out.back().which_payload_variant==meshtastic_FromRadio_config_complete_id_tag);
  assert(client.localModuleLog("GenericThread","clock test",mt::Companion::WorkspaceProof{true}));
  assert(sink.out.back().log_record.time==expected&&sink.wallTime()==utc&&config.localClock(utc)==expected);
}
int main(){
  for(const char *s:{"","UTC0","IST-5:30","<-03>3","CST6CDT,M3.2.0/2,M11.1.0/2","NZST-12NZDT,M9.5.0,M4.1.0/3","XXX0YYY,J60/1,304/3"})assert(LocalTime::valid(s));
  for(const char *s:{"x","America/Chicago","A0","CST25","CST6CDT,M13.2.0,M11.1.0","ABC0DEF,M3.2.0/99,M11.1.0","<+05>","UTC0\n"})assert(!LocalTime::valid(s));
  char label[10];LocalTime::label(0,false,label);assert(!strcmp(label,"--:--"));
  LocalTime::label(86400,true,label);assert(!strcmp(label,"12:00a"));
  LocalTime::label(86400+12*3600,true,label);assert(!strcmp(label,"12:00p"));
  LocalTime::label(86400+23*3600+7*60,false,label);assert(!strcmp(label,"23:07"));
  const char zone[]="CST6CDT,M3.2.0/2,M11.1.0/2";
  Fixture f(81);auto a=device();strcpy(a.set_config.payload_variant.device.tzdef,zone);
  f.admin(a);assert(f.usb.error()==0&&f.service.config.timezoneEquals(a.set_config.payload_variant.device.tzdef));
  getter(f,true);assert(!strcmp(f.ble.reply().get_config_response.payload_variant.device.tzdef,zone));
  mt::SettingsJournal cold;assert(!cold.load(f.backend)&&cold.config().same(f.service.config));
  auto saved=f.service.config;auto changed=saved;mt::ChannelRecord ch;
  const uint8_t key[32]={17};assert(ch.set(2,"Third",key,sizeof(key)));
  for(unsigned i=2;i<=6;++i){assert(changed.setSecondaryChannel(i,ch));mt::ChannelRecord check;
    assert(changed.channelRecord(i,check)&&!memcmp(ch.bytes,check.bytes,ch.Size));}
  assert(changed.timezoneEquals(a.set_config.payload_variant.device.tzdef));
  changed.resetSecondaryChannels();changed.seal();assert(changed.timezoneEquals(a.set_config.payload_variant.device.tzdef)&&changed.valid());
  strcpy(a.set_config.payload_variant.device.tzdef,"BAD");f.admin(a);assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&f.service.config.same(saved));
  // US/European/southern seasonal rules, fractional offsets, and both sides of
  // the 2024 spring/fall transitions, including the pinned donor's DST convention.
  for(const char *z:{zone,"CET-1CEST,M3.5.0/2,M10.5.0/3","NZST-12NZDT,M9.5.0,M4.1.0/3","IST-5:30"}){
    for(uint32_t t:{1704067200u,1710007199u,1710007200u,1710053999u,1710054000u,1720051200u,1730599199u,1730599200u,1730617199u,1730617200u}){
      const auto expected=donor(z,t);assert(setenv("TZ","GMT0",1)==0);tzset();
      assert(LocalTime::project(z,t)==expected);
    }
  }
  assert(saved.localClock(1704067200)==1704067200u-6*3600&&saved.localClock(1720051200)==1720051200u-5*3600);
  localLog(saved,f,0,0);localLog(saved,f,1720051200,1720051200u-5*3600);
  mt::Config clockOnly;char emptyZone[65]{};
  assert(clockOnly.bytes[2]==24);clockOnly.twelveHourClock(true);
  assert(clockOnly.bytes[2]==25&&clockOnly.setTimezone(emptyZone)&&clockOnly.bytes[2]==25);
  clockOnly.twelveHourClock(false);assert(clockOnly.bytes[2]==24&&clockOnly.valid());
  mt::Config legacy;const uint32_t filters[]={0xabcdef01u};assert(legacy.setIgnoredSenders(filters,1));
  legacy.bytes[2]=24;legacy.seal();mt::Config migrated;
  assert(mt::Config::decode(legacy.bytes,mt::Config::Size,migrated)&&migrated.ignoresIncoming(filters[0]));
  assert(!migrated.twelveHourClock()&&migrated.localClock(1704067200)==1704067200u);
  legacy.bytes[mt::Config::clockOffset(0)]='x';legacy.seal();assert(!mt::Config::decode(legacy.bytes,mt::Config::Size,migrated));
  meshtastic_AdminMessage display{};display.which_payload_variant=meshtastic_AdminMessage_set_config_tag;
  display.set_config.which_payload_variant=meshtastic_Config_display_tag;
  display.set_config.payload_variant.display.screen_on_secs=f.service.config.displaySeconds();
  display.set_config.payload_variant.display.use_12h_clock=true;f.admin(display);
#if defined(CONFIG_MT_XIAO_EXPANSION_OLED)
  assert(!f.usb.error()&&f.service.config.twelveHourClock());
  mt::SettingsJournal after;assert(!after.load(f.backend)&&after.config().twelveHourClock()&&after.config().localClock(1720051200)==1720051200u-5*3600);
#else
  assert(f.usb.error()==meshtastic_Routing_Error_BAD_REQUEST&&!f.service.config.twelveHourClock());
#endif
  std::cout<<"PASS local device set/get, verified restart, frozen UTC and local PhoneAPI logs, channel padding, seasonal/DST rules, display capability and V24 migration\n";
}
