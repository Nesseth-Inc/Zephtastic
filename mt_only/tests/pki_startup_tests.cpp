// SPDX-License-Identifier: GPL-3.0-only
#include "pki_runtime_fixture.h"
#include "known_channel_fixture.h"
#include "pki/PkiStartup.h"
#include "NormalTxStartup.h"
struct ReadOnlyBoot:BootSettings {
  Memory &memory;bool held=false;unsigned begins=0,ends=0;
  explicit ReadOnlyBoot(Memory &m):memory(m){}
  int begin()override{if(held)return -EBUSY;held=true;++begins;return 0;}
  void end()override{held=false;++ends;}
  int read(const char *n,uint8_t *p,size_t cap)override{return held?memory.read(n,p,cap):-EACCES;}
};
struct BootContext:PkiStorageContext {
  Radio &radio;mt::Service *service=nullptr;bool held=false;
  explicit BootContext(Radio &r):radio(r){}
  bool tryEnter()override{if(held)return false;held=true;return true;}
  void leave()override{held=false;}
  bool bootQuiescent()const override{return held&&radio.healthValue==0&&service&&service->storageQuiescent();}
  bool quiescent()const override{return held&&service&&service->privateStoragePaused();}
};
struct BootFixture {
  Key local{11},peer{97},other{61};Memory memory;ReadOnlyBoot reader{memory};Radio radio;BootContext context{radio};
  Entropy entropy;SignEntropy signing;Backend backend;mt::experimental::PkiCrypto crypto{backend};
  PkiStorage storage{reader,memory,context,entropy};PkiStartup startup{storage,crypto,backend,signing};
  mt::test::KnownChannelFixtureClock knownChannelClock;
  mt::Service service{radio,true,0,81,true,false};unsigned initialWrites=0;
  explicit BootFixture(unsigned mode=0) {
    knownChannelClock.bind(service);
    context.service=&service;mt::Config cfg;cfg.bytes[4]=1;auto n=cfg.node();n.setPublic();cfg.node(n);assert(service.apply(cfg));
    if(mode!=1){
      IdentityJournal identity;MonocypherIdentityDeriver d;assert(!identity.load(memory,d));
      if(mode!=3){IdentityCandidate c(local.binding,local.scalar);assert(!identity.store(memory,d,c,true));}
      if(mode!=2){PkiLedger ledger;assert(!ledger.load(memory));const auto &b=mode==5?other.binding:local.binding;
        assert(!ledger.initialize(memory,b.publicKey,b.node,true));
        if(mode!=6)assert(!ledger.addTrustedPeer(memory,peer.binding.node,peer.binding.publicKey,true));
        if(mode==7||mode==10)assert(!ledger.addTrustedPeer(memory,other.binding.node^(mode==10?1u:0u),other.binding.publicKey,true));
        if(mode==6||mode==8)assert(!ledger.reserve(memory,701,true));
        if(mode==8){assert(!ledger.commitAuthenticated(memory,peer.binding.node,peer.binding.publicKey,702,true));
          assert(!ledger.revokeTrustedPeer(memory,peer.binding.node,peer.binding.publicKey,true));}
        if(mode==9){assert(!ledger.revokeTrustedPeer(memory,peer.binding.node,peer.binding.publicKey,true));
          assert(!ledger.addTrustedPeer(memory,other.binding.node,other.binding.publicKey,true));}}
      if(mode==4)memory.values["mtidentity/state0"][0]^=1;
    }
    initialWrites=memory.writes;
  }
  bool prepare(){uint32_t node=0x12345678;bool ok=startup.prepare(node);service.route.configureContinuous(uint8_t(node),8);service.route.configureIdentity(node);service.discovery.identity(node);return ok;}
  void unchanged(){assert(memory.writes==initialWrites&&!entropy.calls&&!signing.calls&&!radio.starts&&!radio.sends);}
};
static void group(const char *s){std::cout<<"PASS "<<s<<"\n";}
// Qwen startup-ignore-peer.cpp, with exact refusal/success result assertions.
static void ignoredStoredPeer(){BootFixture f(7);assert(f.prepare());
  const uint32_t self=f.service.route.identity();
  assert(f.service.nodes.edit(f.peer.binding.node,self,mt::NodeCache::Edit::Ignore)==1);
  assert(f.service.nodes.ignored(f.peer.binding.node)&&!f.service.nodes.ignored(f.other.binding.node));
  assert(f.startup.activate(f.service,true)&&f.startup.loadedPeers()==2);
  assert(f.startup.permitsCompanionRadioBoot(true)&&f.startup.permitsCompanionRadioBoot(false));
  assert(f.service.start(false,0)&&f.service.armEndpoint(true));auto d=data();
  assert(f.service.submitPrivate(0,f.peer.binding.node,81,d.data(),d.size(),0,false,nullptr,0,false,100)==-EPERM);
  assert(f.service.submitPrivate(0,f.other.binding.node,82,d.data(),d.size(),0,false,nullptr,0,false,100)==0);
  assert(!f.radio.sends);
  group("locally ignored stored peer stays pinned through activation while private TX is refused");}
// Reopen the persisted LoRa settings on cold startup, without a USB config setter.
static void publicCompanionBoot(uint32_t region,bool txEnabled) {
  BootFixture f(1);
  assert(!f.startup.permitsCompanionRadioBoot(true));
  auto config=f.service.config;
  auto radio=config.radioRequest();radio.regionCode=region;
  assert(config.setRadio(radio));config.setTxEnabled(txEnabled);
  mt::SettingsJournal saved;
  assert(!saved.load(f.memory)&&!saved.save(f.memory,config));
  mt::SettingsJournal reboot;
  assert(!reboot.load(f.memory)&&reboot.config().same(config));
  assert(f.prepare()&&f.startup.state()==PkiStartup::State::Empty);
  assert(f.service.apply(reboot.config())&&f.startup.activate(f.service,false));
  assert(!f.service.privateConfigured()&&!f.startup.permitsCompanionRadioBoot(false));
  const bool normalBoot=f.startup.permitsCompanionRadioBoot(true);
  assert(normalBoot&&mt::NormalTxStartup::activate(f.service,normalBoot,true,100));
  assert(f.service.receiving()==(region!=0)&&f.service.endpointArmed()==(region!=0&&txEnabled));
  assert(f.radio.starts==(region!=0?1u:0u)&&!f.radio.sends);
  if(!f.service.endpointArmed())return;

  Host usb;mt::companion::CompanionWorkspace workspace;mt::Companion phone(f.service,usb,81,workspace);
  meshtastic_ToRadio hello{};hello.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;hello.want_config_id=12;
  input(phone,hello,110);
  meshtastic_ToRadio query{};query.which_payload_variant=meshtastic_ToRadio_packet_tag;
  auto &packet=query.packet;packet.to=0x01234567;packet.id=456;
  packet.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
  packet.decoded.portnum=meshtastic_PortNum_TRACEROUTE_APP;packet.decoded.want_response=true;
  input(phone,query,150);f.service.poll(450,700);
  assert(f.radio.frames.size()==1);
  const auto &frame=f.radio.frames.front();bench::RouteMetadata route;
  assert(f.service.codec.decode(frame.data(),frame.size(),route)&&route.traceRouteValid&&route.wantResponse&&
    route.portnum==meshtastic_PortNum_TRACEROUTE_APP&&bench::le32(frame.data())==packet.to&&
    bench::le32(frame.data()+4)==f.service.route.identity()&&bench::le32(frame.data()+8)==packet.id&&frame[12]==0);
}
int main(){
  publicCompanionBoot(0,false);publicCompanionBoot(1,false);publicCompanionBoot(1,true);
  group("XIAO public companion restores persisted RF and sends a direct USB trace after cold boot; UNSET and TX-off remain off");
  {BootFixture f(1);assert(f.prepare()&&f.startup.state()==PkiStartup::State::Empty&&f.service.route.identity()==0x12345678);
    assert(f.startup.activate(f.service,false)&&!f.service.privateConfigured());f.unchanged();assert(f.service.start(false,0));
    group("empty private namespaces preserve existing Public identity without entropy or writes");}
  for(unsigned mode:{2u,3u,4u,5u,10u}){BootFixture f(mode);assert(!f.prepare());assert(!f.startup.hold(f.service));
    assert(!f.startup.permitsCompanionRadioBoot(true)&&!f.startup.permitsCompanionRadioBoot(false));
    assert(f.service.faulted()&&!f.service.start(false,0)&&!f.service.armEndpoint(true)&&!f.service.privateConfigured());f.unchanged();}
  group("partial corrupt mismatched missing and invalid peer node/key binding never downgrade or arm");
  for(unsigned mode:{6u,8u}){
    BootFixture f(mode);const auto original=f.memory.values;
    assert(f.prepare()&&f.service.route.identity()==f.local.binding.node);
    assert(!f.startup.permitsCompanionRadioBoot(true)&&f.startup.activate(f.service,true));
    assert(f.startup.permitsCompanionRadioBoot(true)&&f.startup.permitsCompanionRadioBoot(false));
    assert(f.startup.state()==PkiStartup::State::Dormant&&!f.startup.loadedPeers()&&
      f.service.privateConfigured()&&f.service.privateDormant()&&!f.service.faulted());
    assert(f.storage.state()==PkiStorage::State::Active&&f.storage.runtimeIdentity());
    IdentityBinding expected;PkiLedger::Status before,after;
    assert(f.storage.preparedBinding(expected)&&expected.node==f.local.binding.node&&
      !memcmp(expected.publicKey,f.local.binding.publicKey,32)&&f.storage.ledgerStatus(before));
    assert(!before.active&&before.sent==1&&before.received==(mode==8?1:0));
    const unsigned writesBeforeStart=f.memory.writes;
    assert(f.service.start(false,0)&&f.service.armEndpoint(true));
    assert(f.memory.writes==writesBeforeStart&&f.service.receiving()&&
      f.service.endpointArmed()&&!f.service.faulted()&&f.radio.healthValue==1);
    const auto plain=data();uint8_t frame[255]{};size_t size=0;
    const unsigned privateWrites=f.memory.writes,privateEntropy=f.entropy.calls;
    const unsigned privateSign=f.signing.calls,privateEncrypt=f.backend.encrypts;
    assert(f.service.submitPrivate(0,f.peer.binding.node,710,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
    assert(f.memory.writes==privateWrites&&f.entropy.calls==privateEntropy&&
      f.signing.calls==privateSign&&f.backend.encrypts==privateEncrypt&&!f.radio.sends&&
      f.memory.values==original);
    PkiLedger::Status publicBefore,publicAfter;
    assert(f.storage.ledgerStatus(publicBefore)&&publicBefore.sent==before.sent);
    const unsigned publicWrites=f.memory.writes,publicSign=f.signing.calls,publicEncrypt=f.backend.encrypts,publicEntropy=f.entropy.calls;
    assert(f.service.encodePublicMessage(0xffffffffu,711,plain.data(),plain.size(),frame,sizeof(frame),size));
    bench::RouteMetadata publicMetadata;
    assert(size>=17&&f.service.codec.decode(frame,size,publicMetadata)&&
      bench::le32(frame+4)==f.local.binding.node&&bench::le32(frame)==0xffffffffu&&
      publicMetadata.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP&&
      publicMetadata.signatureLength==64&&!publicMetadata.publicKeyLength);
    bench::MtSignature verifier;
    assert(verifier.trustPublicKey(f.local.binding.node,f.local.binding.publicKey,100)&&
      verifier.apply(frame,size,publicMetadata,100)==bench::MtSignature::Result::Verified);
    assert(f.memory.writes==publicWrites+1&&f.signing.calls>publicSign&&
      f.backend.encrypts==publicEncrypt&&!f.radio.sends&&f.service.receiving()&&
      f.service.endpointArmed()&&!f.service.faulted()&&f.radio.healthValue==1);
    assert(f.storage.ledgerStatus(publicAfter)&&publicAfter.sent==publicBefore.sent+1&&
      publicAfter.received==publicBefore.received&&publicAfter.stored==publicBefore.stored);
    const auto identity=f.memory.values.at("mtidentity/state0");const auto writes=f.memory.writes;
    const auto beforeAppend=f.memory.values;
    assert(f.service.beginPrivateAdministration()&&f.service.privateStoragePaused());
    PkiPeerPin added;added.node=f.other.binding.node;memcpy(added.publicKey,f.other.binding.publicKey,32);
    assert(!f.storage.addTrustedPeer(expected,added)&&f.service.endPrivateAdministration());
    assert(f.memory.writes==writes+1&&f.memory.values.at("mtidentity/state0")==identity&&
      f.storage.ledgerStatus(after)&&after.generation==publicAfter.generation+1&&
      after.sent==publicAfter.sent&&after.received==before.received&&
      after.stored==before.stored+1&&after.active==1&&
      f.memory.values!=beforeAppend);
    assert(f.service.privateDormant()&&f.startup.state()==PkiStartup::State::Dormant&&!f.startup.loadedPeers());
    const unsigned appendWrites=f.memory.writes,appendEncrypt=f.backend.encrypts;
    assert(f.service.receiving()&&f.service.endpointArmed()&&!f.service.faulted());
    assert(f.service.submitPrivate(0,f.other.binding.node,712,plain.data(),plain.size(),0,false,nullptr,0,false,200)<0&&
      f.memory.writes==appendWrites&&f.backend.encrypts==appendEncrypt&&!f.radio.sends);
    assert(f.entropy.calls==publicEntropy+1&&f.signing.calls>publicSign&&f.radio.starts==1&&!f.radio.sends);
    f.service.stop();
    assert(!f.service.receiving()&&!f.service.endpointArmed()&&f.radio.healthValue==0&&!f.service.faulted());
    assert(f.storage.close());
    // Simulated reboot: fresh storage/runtime objects load the same exact store.
    const auto signingBeforeReboot=f.signing.calls;
    ReadOnlyBoot reader(f.memory);Radio radio;BootContext context(radio);Entropy entropy;
    PkiStorage storage(reader,f.memory,context,entropy);PkiStartup startup(storage,f.crypto,f.backend,f.signing);
    mt::test::KnownChannelFixtureClock knownChannelLocal1;mt::Service service(radio,true,0,81,true,false);knownChannelLocal1.bind(service);context.service=&service;assert(service.apply(f.service.config));
    uint32_t node=0;assert(startup.prepare(node)&&node==f.local.binding.node);
    service.route.configureContinuous(uint8_t(node),8);service.route.configureIdentity(node);service.discovery.identity(node);
    const auto rebootWrites=f.memory.writes;
    assert(startup.activate(service,true)&&startup.state()==PkiStartup::State::Active&&startup.loadedPeers()==1);
    assert(!service.privateDormant()&&storage.active(f.other.binding.node,f.other.binding.publicKey));
    if(mode==8)assert(storage.revoked(f.peer.binding.node)&&!storage.active(f.peer.binding.node,f.peer.binding.publicKey));
    assert(storage.ledgerStatus(after)&&after.sent==publicAfter.sent&&after.received==(mode==8?1:0));
    auto *journal=storage.runtimeIdentity();IdentityBinding binding;KeyRef ref;
    assert(journal&&context.tryEnter()&&journal->binding(binding,ref));context.leave();
    assert(service.beginPrivateAdministration());uint32_t nonce=99;
    assert(!storage.reserve(ref,binding.publicKey,binding.node,701,nonce)&&nonce==99&&service.endPrivateAdministration());
    assert(f.memory.writes==rebootWrites&&f.memory.values.at("mtidentity/state0")==identity&&
      !entropy.calls&&f.signing.calls==signingBeforeReboot&&!radio.starts&&!radio.sends);
    assert(service.start(false,0)&&service.armEndpoint(true));
    group(mode==6?"empty peer domain stays dormant until explicit append and fresh boot, preserving identity and nonce history":
      "all-revoked domain stays dormant until a new explicit pin boots, retaining tombstone and nonce/replay history");
  }
  {BootFixture f(7);assert(f.prepare()&&f.startup.activate(f.service,true)&&f.startup.loadedPeers()==2);
    assert(f.service.start(false,0)&&f.service.armEndpoint(true));auto d=data();
    for(const auto *peer:{&f.peer,&f.other}){
      const uint32_t id=peer==&f.peer?81:82;
      assert(!f.service.submitPrivate(0,peer->binding.node,id,d.data(),d.size(),0,false,nullptr,0,false,100));
    }
    assert(f.entropy.calls==2&&f.service.endpointFree()==2&&!f.radio.sends);
    group("multiple validated pins boot and share the real Service endpoint without default-peer routing");}
  ignoredStoredPeer();
  {BootFixture f(9);assert(f.prepare()&&f.startup.activate(f.service,true));
    assert(f.storage.revoked(f.peer.binding.node)&&f.storage.active(f.other.binding.node,f.other.binding.publicKey));
    group("one active peer remains bootable after a different durable peer tombstone");}
  {BootFixture f;assert(f.prepare()&&f.service.route.identity()==f.local.binding.node);
    assert(f.startup.activate(f.service,true)&&f.startup.state()==PkiStartup::State::Active);
    uint8_t pub[32];assert(f.service.privatePublicIdentity(pub)&&!memcmp(pub,f.local.binding.publicKey,32));f.unchanged();
    assert(!f.service.receiving()&&!f.service.endpointArmed()&&f.storage.runtimeIdentity());
    assert(!f.startup.activate(f.service,true)&&f.service.faulted()&&!f.service.start(false,0));
    group("valid existing binding and single pin activate once with RF off; repeated activation holds");}
  for(unsigned mode=0;mode<4;++mode){BootFixture f;assert(f.prepare());
    if(mode==1)f.service.route.configureIdentity(f.other.binding.node);
    if(mode==2)assert(f.backend.real.close());
    if(mode==3)assert(f.service.start(false,0));
    assert(!f.startup.activate(f.service,mode!=0)&&f.service.faulted()&&!f.service.receiving()&&!f.service.endpointArmed());
    assert(!f.service.start(false,0)&&!f.storage.runtimeIdentity()&&f.memory.writes==f.initialWrites&&!f.entropy.calls&&!f.signing.calls&&!f.radio.sends);}
  group("unready entropy wrong local ID closed backend and live RX hold startup without writes");
  {BootFixture f;assert(f.prepare()&&f.startup.activate(f.service,true)&&f.service.start(false,0)&&f.service.armEndpoint(true));
    f.memory.onWrite=[&]{assert(f.service.privateStoragePaused()&&f.radio.healthValue==0);};auto d=data();
    assert(!f.service.submitPrivate(0,f.peer.binding.node,71,d.data(),d.size(),0,false,nullptr,0,false,100));
    assert(f.entropy.calls==1&&f.memory.writes>f.initialWrites&&f.service.receiving()&&f.service.endpointArmed()&&!f.radio.sends);
    f.service.poll(1000,51);assert(f.radio.sends==1);auto wire=f.radio.frames[0];uint8_t plain[227];size_t n=0;
    assert(f.crypto.decrypt(f.peer.scalar,f.local.binding.publicKey,f.local.binding.node,71,wire.data()+16,wire.size()-16,plain,sizeof(plain),n)&&n==d.size()&&!memcmp(plain,d.data(),n));
    group("target composition authority persists under real Service pause before private ciphertext");}
  {BootFixture f;assert(f.prepare()&&f.startup.activate(f.service,true)&&f.service.start(false,0)&&f.service.armEndpoint(true));
    auto d=data();f.entropy.fail=true;assert(f.service.submitPrivate(0,f.peer.binding.node,72,d.data(),d.size(),0,false,nullptr,0,false,100)<0);
    uint8_t pub[32];assert(f.service.privatePublicIdentity(pub)&&!f.service.faulted()&&f.service.receiving()&&f.service.endpointArmed());
    const auto writes=f.memory.writes;f.entropy.fail=false;assert(f.service.submitPrivate(0,f.peer.binding.node,72,d.data(),d.size(),0,false,nullptr,0,false,101)<0);
    assert(f.entropy.calls==1&&f.memory.writes==writes&&!f.radio.sends);
    assert(!f.service.submitPrivate(0,f.peer.binding.node,73,d.data(),d.size(),0,false,nullptr,0,false,102));
    group("runtime entropy starvation burns ID but does not revoke identity or permit nonce retry");}
  {Fixture f;f.s.stop();Radio radio;mt::test::KnownChannelFixtureClock knownChannelLocal2;mt::Service service(radio,true,f.local.binding.node,81,true,false);knownChannelLocal2.bind(service);
    assert(service.bindPrivateRuntime(f.runtime)&&!service.bindPrivateRuntime(f.runtime));
    group("trusted service boot binding is single assignment");}
}
