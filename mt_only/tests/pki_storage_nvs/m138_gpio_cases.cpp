// SPDX-License-Identifier: GPL-3.0-only
// M138 actual-NVS coexistence fixtures on synthetic byte-addressed NOR only.
#define main m137_pki_storage_cases_main
#include "cases.cpp"
#undef main
#include "modules/GpioModuleJournal.h"

using mt::modules::GpioModuleJournal;
using mt::modules::GpioModuleSettings;

static meshtastic_ModuleConfig m138Remote(bool enabled) {
  meshtastic_ModuleConfig m{};
  m.which_payload_variant = GpioModuleSettings::RemoteTag;
  auto &c = m.payload_variant.remote_hardware;
  c.enabled = enabled;
  c.allow_undefined_pin_access = enabled;
  c.available_pins_count = enabled ? 4 : 0;
  for (unsigned i = 0; i < c.available_pins_count; ++i) {
    auto &p = c.available_pins[i];
    p.gpio_pin = uint8_t(i == 3 ? 63 : i);
    snprintf(p.name, sizeof(p.name), "gpio-%u", i);
    p.type = meshtastic_RemoteHardwarePinType(i % 3);
  }
  return m;
}
static meshtastic_ModuleConfig m138Detection(bool enabled) {
  meshtastic_ModuleConfig m{};
  m.which_payload_variant = GpioModuleSettings::DetectionTag;
  auto &c = m.payload_variant.detection_sensor;
  c.enabled = enabled;
  c.minimum_broadcast_secs = enabled ? UINT32_MAX : 45;
  c.state_broadcast_secs = enabled ? UINT32_MAX - 1 : 0;
  c.send_bell = enabled;
  snprintf(c.name, sizeof(c.name), "sensor");
  c.monitor_pin = enabled ? 63 : 0;
  c.detection_trigger_type =
      meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType(enabled ? 5 : 1);
  c.use_pullup = enabled;
  return m;
}

static void m138Coexistence(unsigned sectors) {
  model_reset(sectors);
  Fixture f; f.provision();
  // Keep the other real production journals populated while measuring this
  // independent namespace. These are the same helpers used by baseline cases.
  mt::modules::CannedMessageJournal canned;
  mt::modules::ExtendedModuleJournal extended;
  mt::modules::StoreForwardJournal sf;
  assert(!canned.load(f.settings) && !extended.load(f.settings) && !sf.load(f.settings));
  char list[201]; memset(list, 'x', 200); list[200] = 0;
  mt::modules::CannedMessageSettings cv;
  assert(cv.assign(list, 200) == mt::modules::CannedMessageSettings::AssignResult::Changed);
  assert(!canned.save(f.settings, cv));
  assert(!extended.save(f.settings, extendedProfile(1)));
  assert(!sf.save(f.settings, storeForwardProfile(16, 1, 1, true, false, true)));
  assert(!sf.save(f.settings, storeForwardProfile(32, 2, 2, true, true, true)));
  PkiStorage storage(f.boot, f.settings, f.context, f.entropy);
  auto ref = f.activate(storage); uint32_t id = 99;
  assert(storage.reserve(ref, f.local.binding.publicKey, f.local.binding.node, 7101, id));
  assert(id == 0x3a3a3a3a);
  assert(storage.commitAuthenticated(f.peer.binding.node, f.peer.binding.publicKey, 8101));
  assert(storage.close());

  static const char *const protectedKeys[] = {
      "mtidentity/state0", "mtidentity/state1", "mtpki/ledger0", "mtpki/ledger1",
      "mtcanned/state0", "mtcanned/state1", "mtextra/state0", "mtextra/state1",
      "mtsf/state0", "mtsf/state1"};
  // settings.read requires capacity for the full stored record. Ledgers are
  // the largest protected records; 512 bytes returned -E2BIG for their valid
  // 1,944-byte production records, which the old fixture mistook for absence.
  static constexpr size_t ProtectedReadCapacity = mt::pki::PkiLedger::RecordSize;
  static_assert(ProtectedReadCapacity >= mt::pki::IdentityJournal::RecordSize &&
                ProtectedReadCapacity >= mt::modules::CannedMessageJournal::RecordSize &&
                ProtectedReadCapacity >= mt::modules::ExtendedModuleJournal::RecordSize &&
                ProtectedReadCapacity >= mt::modules::StoreForwardJournal::RecordSize,
                "protected snapshot buffer must hold every full production record");
  std::array<std::array<uint8_t,ProtectedReadCapacity>,10> protectedValues{};
  std::array<int,10> protectedLengths{};
  for (unsigned i=0;i<10;++i)
    protectedLengths[i]=f.settings.read(protectedKeys[i],protectedValues[i].data(),protectedValues[i].size());
  const unsigned required[]={0,2,4,6,8,9};
  bool protectedRowsPresent=true;
  for(unsigned index:required)protectedRowsPresent=protectedRowsPresent&&protectedLengths[index]>0;
  if(!protectedRowsPresent){
    fprintf(stderr,"M138 protected NVS lengths:");
    for(unsigned i=0;i<10;++i)fprintf(stderr," %s=%d",protectedKeys[i],protectedLengths[i]);
    fputc('\n',stderr);
  }
  assert(protectedRowsPresent);

  GpioModuleJournal journal; const unsigned writes=model_writes();
  assert(!journal.load(f.settings) && journal.state()==GpioModuleJournal::State::Fresh);
  assert(model_writes()==writes);
  assert(!journal.save(f.settings,GpioModuleSettings{}));
  assert(journal.generation()==1&&journal.activeSlot()==0);
  // model_writes counts physical programs/erases, not logical settings saves.
  const unsigned afterFirstSave=model_writes();assert(afterFirstSave>writes);
  assert(journal.save(f.settings,GpioModuleSettings{})==0&&model_writes()==afterFirstSave);
  GpioModuleSettings maximum;
  assert(maximum.set(m138Remote(true))&&maximum.set(m138Detection(true))&&maximum.valid());
  assert(!journal.save(f.settings,maximum));
  assert(journal.generation()==2&&journal.activeSlot()==1);
  model_remount();
  const unsigned readOnlyWrites=model_writes();
  GpioModuleJournal loaded;
  assert(!loaded.load(f.settings)&&loaded.state()==GpioModuleJournal::State::Loaded);
  assert(loaded.generation()==2&&loaded.activeSlot()==1&&loaded.config().same(maximum));
  assert(model_writes()==readOnlyWrites);
  for (unsigned i=0;i<10;++i) {
    std::array<uint8_t,ProtectedReadCapacity> after{};
    assert(f.settings.read(protectedKeys[i],after.data(),after.size())==protectedLengths[i]);
    if (protectedLengths[i]>0)
      assert(!memcmp(after.data(),protectedValues[i].data(),size_t(protectedLengths[i])));
  }
  assertOriginalPki(f);
  printf("PASS %u-sector actual NVS GPIO maximum/two-slot roundtrip coexists with PKI, canned, extended, and StoreForward records; load/no-op are read-only\n",sectors);
}

static void m138WriteCuts() {
  model_reset(4); Fixture f; f.provision();
  GpioModuleJournal journal; assert(!journal.load(f.settings));
  GpioModuleSettings oldValue; assert(oldValue.set(m138Remote(true)));
  assert(!journal.save(f.settings,oldValue));
  GpioModuleSettings newValue=oldValue; assert(newValue.set(m138Detection(true)));
  model_snapshot(); model_fail(0,0);
  assert(!journal.save(f.settings,newValue));
  const unsigned events=model_events(); assert(events>0); model_no_fault();
  unsigned cuts=0;
  for(unsigned event=1;event<=events;++event) for(int phase=0;phase<3;++phase) {
    model_restore(); model_remount();
    GpioModuleJournal attempt; assert(!attempt.load(f.settings));
    assert(attempt.config().same(oldValue));
    model_fail(event,phase);
    const int result=attempt.save(f.settings,newValue);
    if(result) assert(attempt.state()==GpioModuleJournal::State::IoError);
    model_no_fault(); model_remount();
    GpioModuleJournal after; const int loadResult=after.load(f.settings);
    assert(!loadResult || after.state()==GpioModuleJournal::State::Recovered);
    assert(after.config().same(oldValue)||after.config().same(newValue));
    ++cuts;
  }
  printf("PASS M138 production-settings NOR fault fixture: %u finite before/partial/after cuts yield only complete old/new record after remount\n",cuts);
}

static void m138RecoveryAndAmbiguity() {
  model_reset(4); Fixture f; f.provision();
  GpioModuleJournal j; assert(!j.load(f.settings));
  GpioModuleSettings a; assert(a.set(m138Remote(true)));
  GpioModuleSettings b=a; assert(b.set(m138Detection(true)));
  assert(!j.save(f.settings,a) && !j.save(f.settings,b));
  uint8_t slot1[GpioModuleJournal::RecordSize]{};
  assert(f.settings.read("mtgpio/state1",slot1,sizeof(slot1))==int(sizeof(slot1)));
  slot1[sizeof(slot1)-1]^=0x80;
  assert(!f.settings.write("mtgpio/state1",slot1,sizeof(slot1)));
  model_remount();
  const unsigned writes=model_writes();
  GpioModuleJournal recovered;
  assert(!recovered.load(f.settings)&&recovered.state()==GpioModuleJournal::State::Recovered);
  assert(recovered.config().same(a)&&model_writes()==writes);
  assert(!recovered.save(f.settings,a));
  assert(recovered.state()==GpioModuleJournal::State::Loaded&&recovered.generation()==2);

  for (uint32_t delta : {0u, 0x80000000u}) {
    model_reset(4); Fixture ambiguous; ambiguous.provision();
    GpioModuleJournal seed; assert(!seed.load(ambiguous.settings));
    assert(!seed.save(ambiguous.settings,a)&&!seed.save(ambiguous.settings,b));
    uint8_t first[GpioModuleJournal::RecordSize]{},second[GpioModuleJournal::RecordSize]{};
    assert(ambiguous.settings.read("mtgpio/state0",first,sizeof(first))==int(sizeof(first)));
    assert(ambiguous.settings.read("mtgpio/state1",second,sizeof(second))==int(sizeof(second)));
    const uint32_t firstGeneration=uint32_t(first[4])|(uint32_t(first[5])<<8)|
        (uint32_t(first[6])<<16)|(uint32_t(first[7])<<24);
    fixturePut32(second+4,firstGeneration+delta);
    fixturePut32(second+sizeof(second)-4,fixtureCrc32(second,sizeof(second)-4));
    assert(!ambiguous.settings.write("mtgpio/state1",second,sizeof(second)));
    model_remount();
    GpioModuleJournal rejected;
    assert(rejected.load(ambiguous.settings)==-EINVAL);
    assert(rejected.state()==GpioModuleJournal::State::Invalid);
  }
  puts("PASS M138 corrupt-newest fallback is read-only until explicit repair; equal-generation conflict and half-range ambiguity fail closed");
}

static int runM138GpioCases() {
  m137_pki_storage_cases_main();
  m138Coexistence(4);
  m138Coexistence(8);
  m138RecoveryAndAmbiguity();
  m138WriteCuts();
  puts("{\"PASS\":true,\"fixture\":\"M138 GPIO actual NVS coexistence and finite write cuts\",\"hardware\":false}");
  return 0;
}
#ifndef MT_M138_NVS_NO_MAIN
int main() { return runM138GpioCases(); }
#endif
