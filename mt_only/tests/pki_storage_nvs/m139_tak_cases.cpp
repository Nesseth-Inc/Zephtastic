// SPDX-License-Identifier: GPL-3.0-only
// M139 TAK/core migrations atop the complete M138 actual-NVS fixture.
// Reuse the M138 suite's explicit baseline runner, then run these cases.
#define MT_M138_NVS_NO_MAIN
#include "m138_gpio_cases.cpp"
#undef MT_M138_NVS_NO_MAIN

static void m139Put16(uint8_t *p,uint16_t n){p[0]=uint8_t(n);p[1]=uint8_t(n>>8);}
static void m139Put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}

static void m139EncodeCoreV18(const mt::Config &config,uint32_t generation,
                              uint8_t out[mt::SettingsJournal::RecordSize]){
  using mt::SettingsJournal;
  memset(out,0,SettingsJournal::RecordSize);out[0]='Z';out[1]='T';out[2]='J';out[3]=6;
  m139Put32(out+4,generation);m139Put16(out+8,mt::Config::Size);
  memcpy(out+12,config.bytes,mt::Config::Size);
  m139Put32(out+SettingsJournal::RecordSize-4,
            fixtureCrc32(out,SettingsJournal::RecordSize-4));
}

static void m139CoreV18ReadOnly(unsigned sectors){
  model_reset(sectors);Fixture f;f.provision();const auto protectedRows=snapshotProtectedPki(f);
  mt::Config old;old.setDisplayLongNames(true);old.bytes[2]=18;old.bytes[5]=0;old.seal();
  mt::Config direct;assert(mt::Config::decode(old.bytes,sizeof(old.bytes),direct));
  assert(direct.bytes[2]==mt::Config::Version&&!direct.ownerUnmessagable()&&direct.displayLongNames());
  uint8_t legacy[mt::SettingsJournal::RecordSize]{};m139EncodeCoreV18(old,17,legacy);
  assert(!f.settings.write("mtbase/state0",legacy,sizeof(legacy)));
  model_remount();const unsigned writes=model_writes();mt::SettingsJournal loaded;
  assert(!loaded.load(f.settings)&&loaded.state()==mt::SettingsJournal::State::Loaded);
  assert(loaded.generation()==17&&loaded.config().bytes[2]==mt::Config::Version);
  assert(!loaded.config().ownerUnmessagable()&&loaded.config().displayLongNames());
  assert(model_writes()==writes);
  uint8_t actual[mt::SettingsJournal::RecordSize]{};
  assert(f.settings.read("mtbase/state0",actual,sizeof(actual))==int(sizeof(actual)));
  assert(!memcmp(actual,legacy,sizeof(legacy))&&model_writes()==writes);
  assertProtectedPkiUnchanged(f,protectedRows);assertOriginalPki(f);
  printf("PASS M139 %u-sector Config v18 read-only migration preserves bytes and PKI\n",sectors);
}

static void m139OwnerFlagRoundtrip(unsigned sectors){
  model_reset(sectors);Fixture f;f.provision();const auto protectedRows=snapshotProtectedPki(f);
  mt::SettingsJournal journal;assert(!journal.load(f.settings));
  mt::Config candidate=journal.config();candidate.setOwnerUnmessagable(true);assert(candidate.valid());
  assert(!journal.save(f.settings,candidate)&&journal.config().ownerUnmessagable());
  model_remount();const unsigned writes=model_writes();mt::SettingsJournal reopened;
  assert(!reopened.load(f.settings)&&reopened.config().ownerUnmessagable());
  assert(reopened.generation()==1&&model_writes()==writes);
  assertProtectedPkiUnchanged(f,protectedRows);assertOriginalPki(f);
  printf("PASS M139 %u-sector Config v19 owner.is_unmessagable roundtrip\n",sectors);
}

static mt::modules::ExtendedModuleSettings m139TakProfile(
    meshtastic_Team team,meshtastic_MemberRole role){
  using namespace mt::modules;
  ExtendedModuleSettings out;
  assert(out.set(extendedTraffic(73)));
  assert(out.set(extendedCanned(true,true,"_any")));
  assert(out.set(extendedSerial(true,18,19,
      meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_57600,456u,
      meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG)));
  meshtastic_ModuleConfig tak{};tak.which_payload_variant=meshtastic_ModuleConfig_tak_tag;
  tak.payload_variant.tak.team=team;tak.payload_variant.tak.role=role;assert(out.set(tak));
  assert(out.valid());return out;
}
static void m139AssertTakProfile(const mt::modules::ExtendedModuleSettings &settings,
                                 meshtastic_Team team,meshtastic_MemberRole role){
  meshtastic_ModuleConfig out{};
  assert(settings.get(out,meshtastic_ModuleConfig_tak_tag));
  assert(out.payload_variant.tak.team==team&&out.payload_variant.tak.role==role);
  assert(settings.get(out,meshtastic_ModuleConfig_traffic_management_tag));
  assert(out.payload_variant.traffic_management.position_min_interval_secs==73);
  assert(settings.get(out,meshtastic_ModuleConfig_canned_message_tag));
  assert(out.payload_variant.canned_message.enabled&&out.payload_variant.canned_message.send_bell);
  assert(!strcmp(out.payload_variant.canned_message.allow_input_source,"_any"));
  assert(settings.get(out,meshtastic_ModuleConfig_serial_tag));
  assert(out.payload_variant.serial.echo&&out.payload_variant.serial.rxd==18&&
         out.payload_variant.serial.txd==19&&out.payload_variant.serial.timeout==456&&
         out.payload_variant.serial.mode==meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG);
  assert(settings.get(out,meshtastic_ModuleConfig_mesh_beacon_tag));
  assert(out.payload_variant.mesh_beacon.flags==5&&
         out.payload_variant.mesh_beacon.broadcast_interval_secs==0);
}
static void m139TakV3Roundtrip(unsigned sectors){
  using namespace mt::modules;
  model_reset(sectors);Fixture f;f.provision();const auto protectedRows=snapshotProtectedPki(f);
  ExtendedModuleJournal journal;assert(!journal.load(f.settings));
  const auto expected=m139TakProfile(meshtastic_Team_Orange,meshtastic_MemberRole_TeamMember);
  assert(!journal.save(f.settings,expected));m139AssertTakProfile(journal.config(),
      meshtastic_Team_Orange,meshtastic_MemberRole_TeamMember);
  uint8_t raw[ExtendedModuleJournal::RecordSize]{};
  const int n=f.settings.read("mtextra/state0",raw,sizeof(raw));
  assert(n>0&&n<int(sizeof(raw))&&raw[3]==ExtendedModuleJournal::SparseVersion);
  // Both slots are a valid equivalent generation/semantic value only if their
  // generations agree; give slot1 the next generation and repair its wrapper CRC.
  m139Put32(raw+4,journal.generation()+1);
  m139Put32(raw+size_t(n)-4,fixtureCrc32(raw,size_t(n)-4));
  assert(!f.settings.write("mtextra/state1",raw,size_t(n)));
  model_remount();const unsigned writes=model_writes();ExtendedModuleJournal reopened;
  assert(!reopened.load(f.settings)&&reopened.generation()==2);
  m139AssertTakProfile(reopened.config(),meshtastic_Team_Orange,
                       meshtastic_MemberRole_TeamMember);
  assert(model_writes()==writes);assertProtectedPkiUnchanged(f,protectedRows);assertOriginalPki(f);
  printf("PASS M139 %u-sector ExtendedModuleSettings v3 TAK team/role sparse roundtrip\n",sectors);
}

static mt::modules::ExtendedModuleSettings m139LegacyV2(){
  mt::modules::ExtendedModuleSettings old;
  assert(old.set(extendedTraffic(91)));
  assert(old.set(extendedCanned(true,false,"_any")));
  assert(old.set(extendedSerial(false,21,22,
      meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_9600,777u,
      meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE)));
  // V2 did not define TAK bytes. Keep them zero and seal with the old version
  // before wrapping so the production migration must validate this old CRC.
  for(size_t i=mt::modules::ExtendedModuleSettings::TakTeam;i<318;++i)assert(old.bytes[i]==0);
  old.bytes[3]=mt::modules::ExtendedModuleSettings::LegacyV2Version;old.seal();
  return old;
}
static size_t m139DenseExtended(uint32_t generation,
    const mt::modules::ExtendedModuleSettings &inner,uint8_t out[mt::modules::ExtendedModuleJournal::RecordSize]){
  using mt::modules::ExtendedModuleJournal;using mt::modules::ExtendedModuleSettings;
  memset(out,0,ExtendedModuleJournal::RecordSize);out[0]='Z';out[1]='T';out[2]='X';
  out[3]=ExtendedModuleJournal::DenseVersion;m139Put32(out+4,generation);
  m139Put16(out+8,ExtendedModuleSettings::Size);memcpy(out+12,inner.bytes,ExtendedModuleSettings::Size);
  m139Put32(out+ExtendedModuleJournal::RecordSize-4,
            fixtureCrc32(out,ExtendedModuleJournal::RecordSize-4));
  return ExtendedModuleJournal::RecordSize;
}
static size_t m139SparseExtended(uint32_t generation,
    const mt::modules::ExtendedModuleSettings &inner,uint8_t out[mt::modules::ExtendedModuleJournal::RecordSize]){
  using mt::modules::ExtendedModuleJournal;using mt::modules::ExtendedModuleSettings;
  memset(out,0,ExtendedModuleJournal::RecordSize);size_t packed=0;
  for(uint8_t value:inner.bytes)if(value)++packed;
  assert(ExtendedModuleJournal::V2FixedSize+packed+4<ExtendedModuleJournal::RecordSize);
  out[0]='Z';out[1]='T';out[2]='X';out[3]=ExtendedModuleJournal::SparseVersion;
  m139Put32(out+4,generation);m139Put16(out+8,ExtendedModuleSettings::Size);
  m139Put16(out+10,uint16_t(packed));size_t at=ExtendedModuleJournal::V2FixedSize;
  for(size_t i=0;i<ExtendedModuleSettings::Size;++i)if(inner.bytes[i]){
    out[ExtendedModuleJournal::HeaderSize+i/8]|=uint8_t(1u<<(i%8));out[at++]=inner.bytes[i];
  }
  m139Put32(out+at,fixtureCrc32(out,at));return at+4;
}
static void m139AssertLegacyV2Migration(const mt::modules::ExtendedModuleSettings &settings){
  assert(settings.valid()&&settings.bytes[3]==mt::modules::ExtendedModuleSettings::Version);
  meshtastic_ModuleConfig out{};
  assert(settings.get(out,meshtastic_ModuleConfig_tak_tag));
  assert(out.payload_variant.tak.team==meshtastic_Team_Unspecifed_Color&&
         out.payload_variant.tak.role==meshtastic_MemberRole_Unspecifed);
  assert(settings.get(out,meshtastic_ModuleConfig_traffic_management_tag));
  assert(out.payload_variant.traffic_management.position_min_interval_secs==91);
  assert(settings.get(out,meshtastic_ModuleConfig_canned_message_tag));
  assert(out.payload_variant.canned_message.enabled&&!out.payload_variant.canned_message.send_bell);
  assert(!strcmp(out.payload_variant.canned_message.allow_input_source,"_any"));
  assert(settings.get(out,meshtastic_ModuleConfig_serial_tag));
  assert(out.payload_variant.serial.rxd==21&&out.payload_variant.serial.txd==22&&
         out.payload_variant.serial.timeout==777&&
         out.payload_variant.serial.mode==meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE);
  assert(settings.get(out,meshtastic_ModuleConfig_mesh_beacon_tag));
  assert(out.payload_variant.mesh_beacon.flags==5&&
         out.payload_variant.mesh_beacon.broadcast_interval_secs==0);
}
static void m139V2JournalMigration(unsigned sectors,bool sparse){
  using namespace mt::modules;
  model_reset(sectors);Fixture f;f.provision();const auto protectedRows=snapshotProtectedPki(f);
  const auto inner=m139LegacyV2();uint8_t raw[ExtendedModuleJournal::RecordSize]{};
  const size_t n=sparse?m139SparseExtended(29,inner,raw):m139DenseExtended(29,inner,raw);
  assert(!f.settings.write("mtextra/state0",raw,n));const auto savedWrites=model_writes();
  model_remount();const unsigned writes=model_writes();ExtendedModuleJournal loaded;
  assert(!loaded.load(f.settings)&&loaded.state()==ExtendedModuleJournal::State::Loaded);
  assert(loaded.generation()==29);m139AssertLegacyV2Migration(loaded.config());
  assert(model_writes()==writes&&writes==savedWrites);
  uint8_t actual[ExtendedModuleJournal::RecordSize]{};
  assert(f.settings.read("mtextra/state0",actual,sizeof(actual))==int(n));
  assert(!memcmp(actual,raw,n)&&model_writes()==writes);
  assertProtectedPkiUnchanged(f,protectedRows);assertOriginalPki(f);
  printf("PASS M139 %u-sector inner-v2 migration in %s wrapper is RAM-only\n",
         sectors,sparse?"sparse-v2":"dense-v1");
}

static void m139RunAll(){
  for(unsigned sectors:{4u,8u}){
    m139CoreV18ReadOnly(sectors);m139OwnerFlagRoundtrip(sectors);
    m139TakV3Roundtrip(sectors);m139V2JournalMigration(sectors,false);
    m139V2JournalMigration(sectors,true);
  }
  puts("{\"PASS\":true,\"fixture\":\"M139 TAK settings and read-only migrations; includes M138 suite\",\"hardware\":false}");
}

int main(){
  assert(runM138GpioCases()==0);
  m139RunAll();
  return 0;
}
