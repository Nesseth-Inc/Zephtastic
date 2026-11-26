// SPDX-License-Identifier: GPL-3.0-only
// Deterministic public test scalars, synthetic byte-array flash only.
#include "pki/ZephyrPkiStorage.h"
#include "modules/CannedMessageJournal.h"
#include "modules/ExtendedModuleJournal.h"
#include "modules/StoreForwardJournal.h"
#include "MtSignature.h"
#include "monocypher.h"
#include "bridge.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <array>
using namespace mt::pki;
struct Context:PkiStorageContext {
  bool boot=true,runtime=true;unsigned depth=0;
  bool tryEnter()override{++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool bootQuiescent()const override{return depth&&boot;}
  bool quiescent()const override{return depth&&runtime;}
};
struct Entropy:EntropySource {
  unsigned calls=0;bool fail=false;
  bool fill(uint8_t *p,size_t n)override{++calls;memset(p,0x3a,n);return !fail;}
};
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+23*i);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  ~Key(){eraseIdentityBytes(scalar,32);}
  IdentityCandidate candidate(){return IdentityCandidate(binding,scalar);}
};
struct Fixture {
  mt::ZephyrSettings settings;NvsBootSettings boot;Context context;Entropy entropy;
  Key local{19},peer{71};MonocypherIdentityDeriver deriver;
  void provision(){
    IdentityJournal identity;PkiLedger ledger;
    assert(!identity.load(settings,deriver));assert(!identity.store(settings,deriver,local.candidate(),true));
    assert(!ledger.load(settings));assert(!ledger.initialize(settings,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(settings,peer.binding.node,peer.binding.publicKey,true));
  }
  KeyRef activate(PkiStorage &storage){
    unsigned writes=model_writes(),reads=model_reads();assert(storage.prepare()==0);
    // Full namespace scan plus the identity and two present ledger values.
    constexpr unsigned prepareReadLimit=1+2*NvsBootSettings::NameLimit+3;
    assert(model_writes()==writes&&model_reads()-reads<=prepareReadLimit);
    IdentityBinding b;uint32_t p=0;assert(storage.preparedBinding(b)&&b.node==local.binding.node);
    assert(storage.preparedPeer(p)&&p==peer.binding.node);assert(!storage.runtimeIdentity());
    assert(storage.activate(local.binding.node)==0);KeyRef ref;
    assert(storage.runtimeIdentity()->binding(b,ref));return ref;
  }
};
void pass(const char *message){printf("PASS %s\n",message);}
void rawrow(unsigned slot,const void *name,size_t nameSize,const void *value,size_t valueSize){
  assert(model_put(uint16_t(0x8000+slot),name,nameSize)==ssize_t(nameSize));
  assert(model_put(uint16_t(0xc000+slot),value,valueSize)==ssize_t(valueSize));model_counter(uint16_t(0x8000+slot));
}
void readonlyRejected(int expected=-EILSEQ){
  NvsBootSettings boot;unsigned writes=model_writes();std::array<uint8_t,32768> before,after;
  model_copy_flash(before.data());assert(boot.begin()==expected);model_copy_flash(after.data());
  assert(before==after&&writes==model_writes());assert(boot.write("mtpki/ledger0",nullptr,0)==-EACCES);
}
static uint32_t fixtureCrc32(const uint8_t *p,size_t n){
  uint32_t c=UINT32_MAX;
  for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
  return ~c;
}
static void fixturePut32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
static meshtastic_ModuleConfig extendedTraffic(uint32_t n){
  meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_traffic_management_tag;
  auto &t=m.payload_variant.traffic_management;t.position_min_interval_secs=n;t.nodeinfo_direct_response_max_hops=n+1;
  t.rate_limit_window_secs=n+2;t.rate_limit_max_packets=n+3;t.unknown_packet_threshold=n+4;return m;
}
static meshtastic_ModuleConfig extendedCanned(bool enabled,bool bell,const char *source){
  meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_canned_message_tag;
  auto &c=m.payload_variant.canned_message;c.enabled=enabled;c.send_bell=bell;
  if(source)strncpy(c.allow_input_source,source,sizeof(c.allow_input_source)-1);
  return m;
}
static meshtastic_ModuleConfig extendedSerial(bool echo,uint32_t rxd,uint32_t txd,
                                               meshtastic_ModuleConfig_SerialConfig_Serial_Baud baud,
                                               uint32_t timeout,meshtastic_ModuleConfig_SerialConfig_Serial_Mode mode){
  meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_serial_tag;
  auto &s=m.payload_variant.serial;s.enabled=true;s.echo=echo;s.rxd=rxd;s.txd=txd;s.baud=baud;s.timeout=timeout;s.mode=mode;return m;
}
static meshtastic_ModuleConfig extendedBeacon(uint32_t flags,uint32_t interval,const char *message){
  meshtastic_ModuleConfig m{};m.which_payload_variant=meshtastic_ModuleConfig_mesh_beacon_tag;
  auto &b=m.payload_variant.mesh_beacon;b.flags=flags;b.broadcast_interval_secs=interval;
  strncpy(b.broadcast_message,message,sizeof(b.broadcast_message)-1);return m;
}
static mt::modules::ExtendedModuleSettings extendedProfile(unsigned profile){
  using namespace mt::modules;
  ExtendedModuleSettings out;
  const bool a=profile==1;
  assert(out.set(extendedTraffic(a?41:61)));
  assert(out.set(extendedCanned(true,a,a?"_any":"")));
  assert(out.set(extendedSerial(a, a?14:16, a?15:17,
      a?meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_38400:
        meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_115200,
      a?250u:123456u,
      a?meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE:
        meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG)));
  assert(out.set(extendedBeacon(a?5u:7u,a?3600u:7200u,a?"compact-a":"compact-b")));
  out.markSent(a?1700000001u:1700000002u);assert(out.valid());return out;
}
static void assertExtendedRoundtrip(const mt::modules::ExtendedModuleSettings &stored,unsigned profile){
  const bool a=profile==1;assert(stored.same(extendedProfile(profile))&&stored.valid());
  meshtastic_ModuleConfig decoded{};
  assert(stored.get(decoded,meshtastic_ModuleConfig_canned_message_tag));
  assert(decoded.payload_variant.canned_message.enabled&&decoded.payload_variant.canned_message.send_bell==a);
  assert(!strcmp(decoded.payload_variant.canned_message.allow_input_source,a?"_any":""));
  assert(stored.get(decoded,meshtastic_ModuleConfig_serial_tag));
  const auto &serial=decoded.payload_variant.serial;
  assert(serial.enabled&&serial.echo==a&&serial.rxd==(a?14u:16u)&&serial.txd==(a?15u:17u));
  assert(serial.baud==(a?meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_38400:
                         meshtastic_ModuleConfig_SerialConfig_Serial_Baud_BAUD_115200));
  assert(serial.timeout==(a?250u:123456u));
  assert(serial.mode==(a?meshtastic_ModuleConfig_SerialConfig_Serial_Mode_SIMPLE:
                         meshtastic_ModuleConfig_SerialConfig_Serial_Mode_TEXTMSG));
  assert(stored.get(decoded,meshtastic_ModuleConfig_traffic_management_tag));
  const auto &traffic=decoded.payload_variant.traffic_management;const uint32_t n=a?41u:61u;
  assert(traffic.position_min_interval_secs==n&&traffic.nodeinfo_direct_response_max_hops==n+1&&
         traffic.rate_limit_window_secs==n+2&&traffic.rate_limit_max_packets==n+3&&
         traffic.unknown_packet_threshold==n+4);
  assert(stored.get(decoded,meshtastic_ModuleConfig_mesh_beacon_tag));
  const auto &beacon=decoded.payload_variant.mesh_beacon;
  assert(beacon.flags==(a?5u:7u)&&beacon.broadcast_interval_secs==(a?3600u:7200u));
  assert(!strcmp(beacon.broadcast_message,a?"compact-a":"compact-b"));
  assert(stored.sentBefore()&&stored.lastSentUtc()==(a?1700000001u:1700000002u));
}
static void assertOriginalPki(Fixture &f){
  const unsigned writes=model_writes();PkiStorage storage(f.boot,f.settings,f.context,f.entropy);
  (void)f.activate(storage);assert(storage.boundTo(f.local.binding.publicKey,f.local.binding.node));
  assert(storage.active(f.peer.binding.node,f.peer.binding.publicKey));uint8_t pub[32]{};
  assert(storage.lookup(f.peer.binding.node,pub)&&!memcmp(pub,f.peer.binding.publicKey,sizeof(pub)));
  assert(storage.close()&&model_writes()==writes);
}
struct ProtectedPkiRows {
  std::array<uint8_t,mt::pki::IdentityJournal::RecordSize> identity0{};
  std::array<uint8_t,mt::pki::PkiLedger::RecordSize> ledger0{},ledger1{};
  int identity0Length=-ENOENT,ledger0Length=-ENOENT,ledger1Length=-ENOENT;
};
static ProtectedPkiRows snapshotProtectedPki(Fixture &f){
  ProtectedPkiRows rows;
  rows.identity0Length=f.settings.read("mtidentity/state0",rows.identity0.data(),rows.identity0.size());
  rows.ledger0Length=f.settings.read("mtpki/ledger0",rows.ledger0.data(),rows.ledger0.size());
  rows.ledger1Length=f.settings.read("mtpki/ledger1",rows.ledger1.data(),rows.ledger1.size());
  assert(rows.identity0Length>0&&rows.ledger0Length>0&&rows.ledger1Length>0);
  return rows;
}
static void assertProtectedPkiUnchanged(Fixture &f,const ProtectedPkiRows &before){
  const auto after=snapshotProtectedPki(f);
  assert(after.identity0Length==before.identity0Length&&after.ledger0Length==before.ledger0Length&&
         after.ledger1Length==before.ledger1Length);
  assert(after.identity0==before.identity0&&after.ledger0==before.ledger0&&after.ledger1==before.ledger1);
}
static mt::modules::StoreForwardSettings storeForwardProfile(uint32_t records,uint32_t returnMax,
                                                              uint32_t returnWindow,bool enabled,
                                                              bool heartbeat,bool server){
  mt::modules::StoreForwardSettings settings;
  meshtastic_ModuleConfig_StoreForwardConfig raw{};raw.enabled=enabled;raw.heartbeat=heartbeat;
  raw.is_server=server;raw.records=records;raw.history_return_max=returnMax;
  raw.history_return_window=returnWindow;assert(settings.set(raw));return settings;
}
static void assertStoreForwardProfile(const mt::modules::StoreForwardSettings &stored,
                                      const mt::modules::StoreForwardSettings &expected){
  assert(stored.valid()&&stored.same(expected));
  meshtastic_ModuleConfig_StoreForwardConfig raw{};
  assert(stored.get(raw));
  assert(raw.enabled&&raw.is_server);
  assert(raw.heartbeat==(expected.normalized().heartbeat));
  assert(raw.records==expected.normalized().records);
  assert(raw.history_return_max==expected.normalized().historyReturnMax);
  assert(raw.history_return_window==expected.normalized().historyReturnWindow);
}
static void storeForwardJournalNvsCases(unsigned sectors){
  using mt::modules::StoreForwardJournal;using mt::modules::StoreForwardSettings;
  model_reset(sectors);Fixture f;f.provision();const ProtectedPkiRows original=snapshotProtectedPki(f);
  const auto profileA=storeForwardProfile(96,12,240,true,false,true);
  const auto profileB=storeForwardProfile(144,25,60,true,true,true);
  StoreForwardJournal fresh;unsigned writes=model_writes();
  assert(!fresh.load(f.settings)&&fresh.state()==StoreForwardJournal::State::Fresh);
  assert(model_writes()==writes);assert(!fresh.save(f.settings,profileA));
  assert(fresh.state()==StoreForwardJournal::State::Loaded&&fresh.generation()==1&&fresh.activeSlot()==0);
  assertStoreForwardProfile(fresh.config(),profileA);
  uint8_t slot0[StoreForwardJournal::RecordSize]{},slot1[StoreForwardJournal::RecordSize]{};
  assert(f.settings.read("mtsf/state0",slot0,sizeof(slot0))==int(sizeof(slot0)));
  assert(slot0[0]=='Z'&&slot0[1]=='T'&&slot0[2]=='S'&&slot0[3]==1&&slot0[4]==1);
  assert(slot0[8]==StoreForwardSettings::Size&&slot0[9]==0&&slot0[10]==0&&slot0[11]==0);
  assert(f.settings.read("mtsf/state1",slot1,sizeof(slot1))==-ENOENT);
  assertProtectedPkiUnchanged(f,original);assertOriginalPki(f);

  model_remount();
  {writes=model_writes();StoreForwardJournal loaded;
   assert(!loaded.load(f.settings)&&loaded.state()==StoreForwardJournal::State::Loaded);
   assert(loaded.generation()==1&&loaded.activeSlot()==0);assertStoreForwardProfile(loaded.config(),profileA);
   assert(model_writes()==writes);assertProtectedPkiUnchanged(f,original);assertOriginalPki(f);}

  StoreForwardJournal second;assert(!second.load(f.settings));writes=model_writes();
  assert(!second.save(f.settings,profileB));
  assert(model_writes()>writes);
  assert(second.state()==StoreForwardJournal::State::Loaded&&second.generation()==2&&second.activeSlot()==1);
  assertStoreForwardProfile(second.config(),profileB);
  assert(f.settings.read("mtsf/state0",slot0,sizeof(slot0))==int(sizeof(slot0)));
  assert(f.settings.read("mtsf/state1",slot1,sizeof(slot1))==int(sizeof(slot1)));
  assert(slot1[0]=='Z'&&slot1[1]=='T'&&slot1[2]=='S'&&slot1[3]==1&&slot1[4]==2);
  assertProtectedPkiUnchanged(f,original);assertOriginalPki(f);

  model_remount();
  {writes=model_writes();StoreForwardJournal loaded;
   assert(!loaded.load(f.settings)&&loaded.state()==StoreForwardJournal::State::Loaded);
   assert(loaded.generation()==2&&loaded.activeSlot()==1);assertStoreForwardProfile(loaded.config(),profileB);
   assert(model_writes()==writes);assertProtectedPkiUnchanged(f,original);assertOriginalPki(f);}

  // Corrupt the newest slot's CRC. The older valid slot must recover read-only.
  assert(f.settings.read("mtsf/state1",slot1,sizeof(slot1))==int(sizeof(slot1)));
  slot1[StoreForwardJournal::RecordSize-1]^=0x80;
  assert(!f.settings.write("mtsf/state1",slot1,sizeof(slot1)));model_remount();
  {writes=model_writes();StoreForwardJournal recovered;
   assert(!recovered.load(f.settings)&&recovered.state()==StoreForwardJournal::State::Recovered);
   assert(recovered.generation()==1&&recovered.activeSlot()==0);assertStoreForwardProfile(recovered.config(),profileA);
   assert(model_writes()==writes);assertProtectedPkiUnchanged(f,original);assertOriginalPki(f);}
  printf("PASS %u-sector production NVS StoreForward 40B slots alternate/remount, CRC fallback is read-only, and original identity/ledger bytes remain exact\n",sectors);
}
static void compactJournalNvsCases(unsigned sectors){
  using mt::modules::CannedMessageJournal;using mt::modules::CannedMessageSettings;
  using mt::modules::ExtendedModuleJournal;using mt::modules::ExtendedModuleSettings;
  model_reset(sectors);Fixture f;f.provision();const ProtectedPkiRows protectedPki=snapshotProtectedPki(f);
  CannedMessageJournal canned;ExtendedModuleJournal extended;
  assert(!canned.load(f.settings)&&canned.state()==CannedMessageJournal::State::Fresh);
  assert(!extended.load(f.settings)&&extended.state()==ExtendedModuleJournal::State::Fresh);
  assert(!canned.save(f.settings,CannedMessageSettings{}));
  uint8_t cannedRaw[CannedMessageJournal::LegacyRecordSize]{};
  assert(f.settings.read("mtcanned/state0",cannedRaw,sizeof(cannedRaw))==32);
  assert(cannedRaw[0]=='Z'&&cannedRaw[1]=='T'&&cannedRaw[2]=='C'&&cannedRaw[3]==2);
  const auto profileA=extendedProfile(1);assert(!extended.save(f.settings,profileA));
  assertExtendedRoundtrip(extended.config(),1);
  uint8_t extendedRaw[ExtendedModuleJournal::RecordSize]{};
  const int extendedBytes=f.settings.read("mtextra/state0",extendedRaw,sizeof(extendedRaw));
  assert(extendedBytes>0&&extendedBytes<int(ExtendedModuleJournal::RecordSize));
  assert(extendedRaw[0]=='Z'&&extendedRaw[1]=='T'&&extendedRaw[2]=='X'&&extendedRaw[3]==ExtendedModuleJournal::SparseVersion);
  assertOriginalPki(f);

  model_remount();
  {const unsigned writes=model_writes();CannedMessageJournal c;ExtendedModuleJournal e;
   assert(!c.load(f.settings)&&!e.load(f.settings));assert(c.config().same(CannedMessageSettings{}));
   assertExtendedRoundtrip(e.config(),1);assert(e.generation()==1&&model_writes()==writes);assertOriginalPki(f);}

  char full[CannedMessageSettings::MaxBytes+1];memset(full,'q',sizeof(full));
  CannedMessageSettings fullList;assert(fullList.assign(full,CannedMessageSettings::MaxBytes)==CannedMessageSettings::AssignResult::Changed);
  CannedMessageJournal c2;assert(!c2.load(f.settings)&&!c2.save(f.settings,fullList));
  assert(f.settings.read("mtcanned/state1",cannedRaw,sizeof(cannedRaw))==216);
  model_remount();
  {const unsigned writes=model_writes();CannedMessageJournal c;ExtendedModuleJournal e;
   assert(!c.load(f.settings)&&!e.load(f.settings));assert(c.config().length()==200&&memcmp(c.config().messages,full,200)==0);
   assertExtendedRoundtrip(e.config(),1);assert(model_writes()==writes);assertOriginalPki(f);}

  const auto profileB=extendedProfile(2);ExtendedModuleJournal e2;assert(!e2.load(f.settings)&&!e2.save(f.settings,profileB));
  assertExtendedRoundtrip(e2.config(),2);
  extendedRaw[0]=0;const int extendedBytesB=f.settings.read("mtextra/state1",extendedRaw,sizeof(extendedRaw));
  assert(extendedBytesB>0&&extendedBytesB<int(ExtendedModuleJournal::RecordSize)&&
         extendedRaw[3]==ExtendedModuleJournal::SparseVersion);
  model_remount();
  {const unsigned writes=model_writes();CannedMessageJournal c;ExtendedModuleJournal e;
   assert(!c.load(f.settings)&&!e.load(f.settings));assert(c.config().length()==200);assertExtendedRoundtrip(e.config(),2);
   assert(e.generation()==2&&model_writes()==writes);assertOriginalPki(f);}

  CannedMessageJournal c3;assert(!c3.load(f.settings)&&!c3.save(f.settings,CannedMessageSettings{}));
  model_remount();
  {const unsigned writes=model_writes();CannedMessageJournal c;ExtendedModuleJournal e;
   assert(!c.load(f.settings)&&!e.load(f.settings));assert(c.config().same(CannedMessageSettings{}));
   assertExtendedRoundtrip(e.config(),2);assert(model_writes()==writes);assertOriginalPki(f);}

  // Keep both existing compact journal families and their larger profile data
  // live while committing both 40-byte StoreForward slots into the same NVS.
  using mt::modules::StoreForwardJournal;using mt::modules::StoreForwardSettings;
  const auto sfA=storeForwardProfile(180,20,120,true,false,true);
  const auto sfB=storeForwardProfile(280,25,300,true,true,true);
  StoreForwardJournal sf;unsigned sfWrites=model_writes();
  assert(!sf.load(f.settings)&&sf.state()==StoreForwardJournal::State::Fresh);
  assert(model_writes()==sfWrites);assert(!sf.save(f.settings,sfA));assert(!sf.save(f.settings,sfB));
  assert(model_writes()>sfWrites);
  uint8_t sf0[StoreForwardJournal::RecordSize]{},sf1[StoreForwardJournal::RecordSize]{};
  assert(f.settings.read("mtsf/state0",sf0,sizeof(sf0))==int(sizeof(sf0)));
  assert(f.settings.read("mtsf/state1",sf1,sizeof(sf1))==int(sizeof(sf1)));
  assert(sf0[0]=='Z'&&sf0[1]=='T'&&sf0[2]=='S'&&sf0[4]==1);
  assert(sf1[0]=='Z'&&sf1[1]=='T'&&sf1[2]=='S'&&sf1[4]==2);
  assertProtectedPkiUnchanged(f,protectedPki);assertOriginalPki(f);
  model_remount();
  {const unsigned writes=model_writes();StoreForwardJournal loaded;CannedMessageJournal c;ExtendedModuleJournal e;
   assert(!loaded.load(f.settings)&&!c.load(f.settings)&&!e.load(f.settings));
   assert(loaded.state()==StoreForwardJournal::State::Loaded&&loaded.generation()==2&&loaded.activeSlot()==1);
   assertStoreForwardProfile(loaded.config(),sfB);assert(c.config().same(CannedMessageSettings{}));
   assertExtendedRoundtrip(e.config(),2);assert(model_writes()==writes);
   assertProtectedPkiUnchanged(f,protectedPki);assertOriginalPki(f);}

  storeForwardJournalNvsCases(sectors);
  printf("PASS %u-sector production NVS compact journals: canned default 32B/full opaque 200B 216B; sparse extended roundtrip and alternating remounts preserve original PKI identity/ledger; loads write nothing\n",sectors);

  // Independently encode the original dense version-1 record. Loading it may
  // upgrade only the RAM representation; it must not rewrite/migrate NVS.
  model_reset(sectors);Fixture old;old.provision();ExtendedModuleSettings legacy;
  legacy.bytes[3]=ExtendedModuleSettings::LegacyVersion;legacy.seal();assert(legacy.valid()==false);
  uint8_t v1[ExtendedModuleJournal::RecordSize]{};v1[0]='Z';v1[1]='T';v1[2]='X';v1[3]=ExtendedModuleJournal::DenseVersion;
  fixturePut32(v1+4,7);v1[8]=uint8_t(ExtendedModuleSettings::Size);v1[9]=uint8_t(ExtendedModuleSettings::Size>>8);
  memcpy(v1+12,legacy.bytes,ExtendedModuleSettings::Size);fixturePut32(v1+sizeof(v1)-4,fixtureCrc32(v1,sizeof(v1)-4));
  assert(!old.settings.write("mtextra/state0",v1,sizeof(v1)));model_remount();
  {const unsigned writes=model_writes();ExtendedModuleJournal loaded;
   assert(!loaded.load(old.settings)&&loaded.state()==ExtendedModuleJournal::State::Loaded&&loaded.generation()==7);
   assert(loaded.config().bytes[3]==ExtendedModuleSettings::Version&&model_writes()==writes);
   uint8_t actual[ExtendedModuleJournal::RecordSize]{};assert(old.settings.read("mtextra/state0",actual,sizeof(actual))==int(sizeof(v1)));
   assert(!memcmp(actual,v1,sizeof(v1))&&model_writes()==writes);assertOriginalPki(old);}
  puts("PASS production NVS dense-v1 extended fixture loads/migrates in RAM only; exact 336B NVS record and original PKI state remain unchanged");
}
void cases(unsigned sectors){
  model_reset(sectors);
  {Fixture f;PkiStorage storage(f.boot,f.settings,f.context,f.entropy);const auto writes=model_writes();
   assert(storage.prepare()==-ENOENT&&storage.state()==PkiStorage::State::Empty&&!storage.runtimeIdentity());
   assert(model_writes()==writes&&!f.entropy.calls);IdentityBinding out;out.node=91;
   assert(!storage.preparedBinding(out)&&out.node==91);}
  pass("actual NVS absent startup is read-only, no identity created");
  model_reset(sectors);
  {Fixture f;f.provision();PkiStorage storage(f.boot,f.settings,f.context,f.entropy);auto ref=f.activate(storage);
   uint32_t extra=0;assert(storage.reserve(ref,f.local.binding.publicKey,f.local.binding.node,101,extra)&&extra==0x3a3a3a3a);
   assert(storage.commitAuthenticated(f.peer.binding.node,f.peer.binding.publicKey,201));assert(storage.close());model_remount();
   PkiStorage again(f.boot,f.settings,f.context,f.entropy);ref=f.activate(again);extra=17;
   assert(!again.reserve(ref,f.local.binding.publicKey,f.local.binding.node,101,extra)&&extra==17);
   assert(again.seen(f.peer.binding.node,f.peer.binding.publicKey,201)==ReplayCheck::Seen);
   assert(again.revokeTrustedPeer(f.peer.binding.node,f.peer.binding.publicKey));assert(again.close());model_remount();
   PkiStorage revoked(f.boot,f.settings,f.context,f.entropy);assert(!revoked.prepare());uint32_t peer=17;
   assert(!revoked.preparedPeer(peer)&&peer==17);assert(!revoked.activate(f.local.binding.node));
   assert(revoked.revoked(f.peer.binding.node));uint8_t pub[32];memset(pub,0xa5,32);
   assert(!revoked.lookup(f.peer.binding.node,pub));for(uint8_t b:pub)assert(b==0xa5);}
  pass("real settings save/load, nonce/replay/tombstone persist across NVS remount");
  // Every raw flash mutation of an actual ledger reservation, before/partial/after.
  model_reset(sectors);Fixture f;f.provision();model_snapshot();unsigned events=0;
  {PkiStorage storage(f.boot,f.settings,f.context,f.entropy);auto ref=f.activate(storage);model_fail(0,0);uint32_t extra=0;
   assert(storage.reserve(ref,f.local.binding.publicKey,f.local.binding.node,301,extra));events=model_events();}
  assert(events>0);unsigned faultCases=0,committedDespiteMarkerFault=0;
  for(unsigned event=1;event<=events;++event)for(int phase=0;phase<3;++phase){
    model_restore();PkiStorage storage(f.boot,f.settings,f.context,f.entropy);auto ref=f.activate(storage);
    auto *identity=storage.runtimeIdentity();assert(identity);unsigned entropyCalls=f.entropy.calls;model_fail(event,phase);uint32_t extra=99;
    bool reserved=storage.reserve(ref,f.local.binding.publicKey,f.local.binding.node,301,extra);
    if(!reserved){
      assert(extra==99&&f.entropy.calls==entropyCalls);
      assert(storage.state()==PkiStorage::State::Frozen&&!storage.runtimeIdentity()&&!identity->active(ref));
      unsigned writes=model_writes();model_no_fault();assert(!storage.reserve(ref,f.local.binding.publicKey,f.local.binding.node,302,extra));
      assert(model_writes()==writes);
    }else{
      // Pinned nvs_sector_close explicitly ignores close-marker write status.
      // If save+exact-readback succeeded, reboot must retain the burned ID or
      // refuse activation; accepting an older ready snapshot would be unsafe.
      assert(extra==0x3a3a3a3a&&f.entropy.calls==entropyCalls+1);++committedDespiteMarkerFault;
    }
    model_no_fault();model_remount();NvsBootSettings freshBoot;PkiStorage fresh(freshBoot,f.settings,f.context,f.entropy);
    int result=fresh.prepare();assert(result==0||result==-EILSEQ||result==-ENODATA||result==-EIO);
    if(!result){assert(!fresh.activate(f.local.binding.node));IdentityBinding b;KeyRef r;assert(fresh.runtimeIdentity()->binding(b,r));
      bool retry=fresh.reserve(r,b.publicKey,b.node,301,extra);
      if(reserved)assert(!retry); // Never reuse an ID whose nonce was exposed.
      // A reservation that never returned a nonce may be old or new on remount.
    }
    ++faultCases;
  }
  printf("PASS %usector reservation mutations: %u faults; %u close-marker errors hidden by NVS but committed IDs survive; exposed failures freeze\n",sectors,faultCases,committedDespiteMarkerFault);
  model_reset(sectors);f.provision();model_busy(1);
  {NvsBootSettings boot;assert(boot.begin()==-EBUSY);model_busy(0);assert(boot.begin()==0);assert(boot.begin()==-EBUSY);boot.end();}
  pass("existing NVS mutex contention fails without waiting, lease balanced");
  // A missing journal family is partial, never Empty/Public fallback.
  for(unsigned missing=0;missing<2;++missing){model_reset(sectors);
    if(!missing){PkiLedger ledger;assert(!ledger.load(f.settings));
      assert(!ledger.initialize(f.settings,f.local.binding.publicKey,f.local.binding.node,true));}
    else{IdentityJournal identity;assert(!identity.load(f.settings,f.deriver));
      assert(!identity.store(f.settings,f.deriver,f.local.candidate(),true));}
    model_remount();PkiStorage storage(f.boot,f.settings,f.context,f.entropy);unsigned writes=model_writes();
    assert(storage.prepare()==-ENODATA&&storage.state()==PkiStorage::State::Partial);assert(model_writes()==writes);}
  pass("partial identity/ledger families never downgrade to absent");
  // Low-level malformed rows: boot reader never invokes settings repair.
  const uint8_t value=7;const char name[]="other/key";
  for(unsigned variant=0;variant<11;++variant){model_reset(sectors);
    if(variant==0){model_counter(0x8001);assert(model_put(0x8001,name,sizeof(name)-1)>0);}
    if(variant==1){model_counter(0x8001);assert(model_put(0xc001,&value,1)>0);}
    if(variant==2){rawrow(1,name,sizeof(name)-1,&value,1);rawrow(2,name,sizeof(name)-1,&value,1);}
    if(variant==3){const char bad[]={'x',0,'y'};rawrow(1,bad,sizeof(bad),&value,1);}
    if(variant==4){char bad[97];memset(bad,'x',sizeof(bad));rawrow(1,bad,sizeof(bad),&value,1);}
    if(variant==5){model_counter(0x8001);NvsBootSettings boot;unsigned writes=model_writes();
      std::array<uint8_t,32768> before,after;model_copy_flash(before.data());assert(!boot.begin());boot.end();
      model_copy_flash(after.data());assert(before==after&&writes==model_writes());}
    if(variant==6){rawrow(1,name,sizeof(name)-1,&value,1);model_counter(0x8000);}
    if(variant==7){model_counter(uint16_t(0x8000u+NvsBootSettings::NameLimit+1u));}
    if(variant==8){uint8_t count=0x80;assert(model_put(0x8000,&count,1)==1);}
    if(variant==9){rawrow(1,"mtidentity/state0",17,&value,1);}
    if(variant==10){rawrow(1,name,sizeof(name)-1,&value,1);assert(!model_delete(0x8000));}
    if(variant!=5)readonlyRejected();
  }
  pass("partial orphan/duplicate/NUL/oversize/gap/low-count/range/exact-length corruption rejected without repair; empty counter-only tail accepted read-only");
  // A durable name counter may be one ahead of the last completed setting.
  // Keep real identity/ledger rows, fill intervening names, then leave only
  // the allocated final ID absent. The boot scan may ignore that empty tail
  // but must neither rewrite the counter nor change the admitted identity.
  model_reset(sectors);Fixture tail;tail.provision();
  uint16_t counter=0;assert(model_get(0x8000,&counter,sizeof(counter))==ssize_t(sizeof(counter)));
  assert(counter>=0x8000&&counter<0x8014);
  for(unsigned slot=unsigned(counter-0x8000)+1;slot<20;++slot){
    char row[32];const int n=snprintf(row,sizeof(row),"fixture/row%u",slot);assert(n>0);
    rawrow(slot,row,size_t(n),&value,1);
  }
  model_counter(0x8014); // ID 20 allocated in the counter, with no name/value.
  model_remount();
  {NvsBootSettings boot;const unsigned writes=model_writes();std::array<uint8_t,32768> before,after;
   model_copy_flash(before.data());assert(!boot.begin());
   assert(boot.diagnostic().lastNameId==0x8014&&boot.diagnostic().names==19&&boot.diagnostic().stage==9&&boot.diagnostic().valueBytes>0);
   boot.end();model_copy_flash(after.data());
   assert(before==after&&writes==model_writes());
   PkiStorage storage(tail.boot,tail.settings,tail.context,tail.entropy);auto ref=tail.activate(storage);
   IdentityBinding admitted;assert(storage.runtimeIdentity()&&storage.runtimeIdentity()->binding(admitted,ref));
   assert(admitted.node==tail.local.binding.node&&admitted.generation==tail.local.binding.generation&&
          !memcmp(admitted.publicKey,tail.local.binding.publicKey,sizeof(admitted.publicKey)));
   assert(model_writes()==writes);}
  pass("metadata-only allocated empty tail is admitted read-only with original PKI identity and ledger");
  // A partial tail is not a metadata-only allocation and remains corruption.
  for(unsigned tailKind=0;tailKind<2;++tailKind){
    model_reset(sectors);Fixture partial;partial.provision();
    uint16_t current=0;assert(model_get(0x8000,&current,sizeof(current))==ssize_t(sizeof(current)));
    assert(current>=0x8000&&current<0x8014);
    for(unsigned slot=unsigned(current-0x8000)+1;slot<20;++slot){
      char row[32];const int n=snprintf(row,sizeof(row),"fixture/partial%u",slot);assert(n>0);
      rawrow(slot,row,size_t(n),&value,1);
    }
    model_counter(0x8014);
    if(tailKind==0)assert(model_put(0x8014,name,sizeof(name)-1)>0); // name-only
    else assert(model_put(0xc014,&value,1)>0); // value-only
    model_remount();readonlyRejected();
  }
  pass("name-only and value-only allocated tails remain rejected without repair");
  model_reset(sectors);
  for(unsigned i=1;i<=NvsBootSettings::NameLimit;++i){char name[32];int n=snprintf(name,sizeof(name),"other/row%u",i);rawrow(i,name,size_t(n),&value,1);}
  {NvsBootSettings boot;unsigned writes=model_writes(),reads=model_reads();assert(!boot.begin());
   assert(model_reads()-reads==1+2*NvsBootSettings::NameLimit&&model_writes()==writes);boot.end();}
  pass("maximum supported settings namespace scanned in exactly one counter plus two reads per slot");
  model_reset(sectors);f.provision();
  for(uint16_t id:{uint16_t(0x8000),uint16_t(0x8001),uint16_t(0xc001),uint16_t(0x8040),uint16_t(0xc040)}){
    model_read_error(id,-EIO);readonlyRejected(-EIO);model_no_fault();}
  {NvsBootSettings boot;assert(!boot.begin());std::array<uint8_t,84> scratch;scratch.fill(0xa5);
   model_read_error(0xc001,-EIO);assert(boot.read("mtidentity/state0",scratch.data(),scratch.size())==-EIO);
   for(auto byte:scratch)assert(!byte);
   model_no_fault();boot.end();}
  pass("raw read errors never establish absence, failed identity output is cleared");
  compactJournalNvsCases(sectors);
}
int main(){cases(4);cases(8);return 0;}
