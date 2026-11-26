// SPDX-License-Identifier: GPL-3.0-only
// UNCOMPILED M101 proposal: deterministic keys, modeled NOR/NVS and real PSA.
// No target storage, RF barrier, IRQ race, stack peak or hardware claim.
#include "pki_runtime_fixture.h"
#include "pki/PkiAppendHistory.h"
#include "pki/PkiStartup.h"
#include "pki/MultiPeerRuntime.h"
#include <array>

namespace {
struct Nor final:AppendStore {
  Bytes bytes=Bytes(PkiAppendHistory::StoreBytes,0xff);
  unsigned programs=0;std::function<void()> programGuard;
  size_t capacity()const override{return bytes.size();}
  int read(size_t offset,uint8_t *out,size_t size)override{
    if(!out||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    memcpy(out,bytes.data()+offset,size);return 0;
  }
  int program(size_t offset,const uint8_t *data,size_t size)override{
    assert(programGuard);programGuard();
    if(!data||!size||(offset&3)||(size&3)||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    for(size_t i=0;i<size;++i)if((bytes[offset+i]&data[i])!=data[i])return -EIO;
    ++programs;for(size_t i=0;i<size;++i)bytes[offset+i]&=data[i];return 0;
  }
  // AppendStore deliberately exposes no erase/reset/format operation.
};
struct Domain {
  Key local{11},peers[17]{Key{97},Key{61},Key{149},Key{193},Key{227},
    Key{17},Key{41},Key{73},Key{109},Key{137},Key{169},Key{197},
    Key{229},Key{29},Key{53},Key{89},Key{121}};
  Memory nvs;Nor nor;
  Domain(){
    MonocypherIdentityDeriver derive;IdentityJournal identity;PkiLedger ledger;
    assert(!identity.load(nvs,derive));IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(nvs,derive,candidate,true)&&!ledger.load(nvs));
    assert(!ledger.initialize(nvs,local.binding.publicKey,local.binding.node,true));
    for(unsigned i=0;i<4;++i)assert(!ledger.addTrustedPeer(nvs,peers[i].binding.node,peers[i].binding.publicKey,true));
    assert(!ledger.reserve(nvs,51,true));
    for(unsigned i=0;i<4;++i)assert(!ledger.commitAuthenticated(nvs,peers[i].binding.node,peers[i].binding.publicKey,61+i,true));
  }
  PkiPeerPin pin(unsigned i)const{
    assert(i<17);PkiPeerPin p;p.node=peers[i].binding.node;memcpy(p.publicKey,peers[i].binding.publicKey,32);return p;
  }
};
struct Boot final:BootSettings {
  Memory &nvs;bool held=false;unsigned begins=0,ends=0;
  explicit Boot(Memory &m):nvs(m){}
  int begin()override{if(held)return -EBUSY;held=true;++begins;return 0;}
  void end()override{assert(held);held=false;++ends;}
  int read(const char *key,uint8_t *out,size_t size)override{return held?nvs.read(key,out,size):-EACCES;}
};
struct Context final:PkiStorageContext {
  Radio &radio;mt::Service *service=nullptr;bool held=false;
  explicit Context(Radio &r):radio(r){}
  bool tryEnter()override{if(held)return false;held=true;return true;}
  void leave()override{assert(held);held=false;}
  bool bootQuiescent()const override{return held&&service&&service->storageQuiescent()&&radio.healthValue==0;}
  bool quiescent()const override{return held&&service&&service->privateStoragePaused()&&radio.healthValue==0;}
};
struct Running {
  Domain &domain;Boot reader;Radio radio;Context context{radio};Entropy entropy;
  PkiAppendHistory append;PkiStorage storage;SignEntropy signing;Backend backend;
  mt::experimental::PkiCrypto crypto{backend};PkiStartup startup;
  mt::test::KnownChannelFixtureClock clock;
  mt::Service service{radio,true,0,127,true,false};
  mt::companion::CompanionWorkspace m147Workspace1;
  Host usb;mt::Companion companion{service, usb, 127, m147Workspace1, 0};
  explicit Running(Domain &d):domain(d),reader(d.nvs),append(d.nor),
    storage(reader,d.nvs,context,entropy,&append),startup(storage,crypto,backend,signing){
    clock.bind(service);context.service=&service;
    mt::Config cfg;cfg.bytes[4]=1;auto node=cfg.node();node.setPublic();cfg.node(node);
    assert(service.apply(cfg)&&service.requirePrivateRuntime());
    domain.nor.programGuard=[this]{assert(service.privateStoragePaused()&&!radio.healthValue);};
    domain.nvs.onWrite=[this]{assert(service.privateStoragePaused()&&!radio.healthValue);};
  }
  ~Running(){service.stop();assert(storage.close());domain.nor.programGuard={};domain.nvs.onWrite={};}
  void boot(unsigned count){
    uint32_t node=0;assert(startup.prepare(node)&&node==domain.local.binding.node);
    service.route.configureContinuous(uint8_t(node),8);service.route.configureIdentity(node);service.discovery.identity(node);
    assert(startup.activate(service,true)&&startup.loadedPeers()==count);
  }
  void start(){
    assert(service.start(false,0)&&service.armEndpoint(true));
    meshtastic_ToRadio request{};request.which_payload_variant=meshtastic_ToRadio_want_config_id_tag;
    request.want_config_id=1;input(companion,request,1);usb.out.clear();usb.records.clear();
  }
  void pause(){assert(service.beginPrivateAdministration()&&service.privateStoragePaused()&&!radio.healthValue);}
  void resume(){assert(service.endPrivateAdministration());}
  IdentityBinding binding(){IdentityBinding b;assert(storage.preparedBinding(b));return b;}
  void enable(){const auto b=binding();pause();assert(!storage.enableLongStorage(b));resume();assert(storage.extendedStorage());}
  int add(unsigned i){const auto b=binding();pause();const int rc=storage.addTrustedPeer(b,domain.pin(i));resume();return rc;}
  void poll(uint32_t now){assert(clock.set(now));service.poll(now,71);}
  Bytes incoming(unsigned peer,uint32_t id){
    const auto plain=data("contact-cache RX");uint8_t encrypted[239]{};size_t size=0;
    assert(crypto.encrypt(domain.peers[peer].scalar,domain.local.binding.publicKey,
      domain.peers[peer].binding.node,id,0x19374628,plain.data(),plain.size(),encrypted,sizeof(encrypted),size));
    Bytes frame(16+size);put(frame.data(),domain.local.binding.node);put(frame.data()+4,domain.peers[peer].binding.node);
    put(frame.data()+8,id);frame[15]=uint8_t(domain.peers[peer].binding.node);memcpy(frame.data()+16,encrypted,size);return frame;
  }
  void receive(unsigned peer,uint32_t id,uint32_t now,bool fresh){
    auto frame=incoming(peer,id);assert(clock.set(now));service.rx(frame.data(),frame.size(),3,now,71);
    const auto plain=data("contact-cache RX");PrivateEvent event;
    for(uint8_t owner:{0,1}){
      const bool found=service.takePrivateEvent(owner,frame.data(),frame.size(),event);assert(found==fresh);
      if(found)assert(event.from==domain.peers[peer].binding.node&&event.to==domain.local.binding.node&&event.id==id&&
        event.size==plain.size()&&!memcmp(event.data,plain.data(),plain.size())&&
        !memcmp(event.publicKey,domain.peers[peer].binding.publicKey,32));
    }
    assert(!service.takePrivateEvent(0,frame.data(),frame.size(),event));
  }
};
unsigned groups=0;
void pass(const char *name){++groups;std::cout<<"PASS "<<name<<'\n';}
void sixteen(Running &r){
  PkiPeerSet set;assert(r.storage.preparedPeers(set)&&set.count()==16);
  for(unsigned i=0;i<16;++i){const auto *pin=set.find(r.domain.peers[i].binding.node);assert(pin&&!memcmp(pin->publicKey,r.domain.peers[i].binding.publicKey,32));}
}
struct BasicNonces final:NonceAuthority {
  unsigned calls=0;
  bool reserve(KeyRef,const uint8_t *,uint32_t,uint32_t,uint32_t &extra)override{
    extra=0x65430000u+ ++calls;return true; // Deliberately does NOT reject reused IDs.
  }
};
void busyCache(Running &r){
  PkiPeerSet set;assert(r.storage.preparedPeers(set));auto *identity=r.storage.runtimeIdentity();assert(identity);
  MultiPeerRuntime runtime(r.crypto,r.backend,*identity,r.storage,r.storage,&r.signing);
  assert(runtime.open(r.domain.local.binding.node,set));mt::Endpoint endpoint;
  const auto plain=data("protected hot cache");const unsigned peers[4]={0,1,2,15};
  auto send=[&](unsigned peer,uint32_t id,uint8_t owner=0){return runtime.submit(endpoint,owner,r.domain.peers[peer].binding.node,id,plain.data(),plain.size(),0,true,nullptr,0,true,100);};
  // Use the actual Service storage pause for durable nonce mutations. This
  // standalone Endpoint's modeled borrowed buffer is NOT a real native TX.
  r.pause();
  assert(!send(peers[0],70000,1));auto *active=endpoint.next(100);assert(active);
  endpoint.started(*active,0,101,100,0);assert(active->state==mt::Endpoint::State::Active);
  const Bytes borrowed(active->frame.bytes,active->frame.bytes+active->frame.len);
  assert(!send(peers[1],70001));auto *waiting=endpoint.next(100);assert(waiting&&waiting!=active);
  endpoint.started(*waiting,0,101,100,0);endpoint.complete(*waiting,102,0);
  assert(waiting->state==mt::Endpoint::State::Waiting);const auto due=waiting->due;
  assert(!send(peers[2],70002)&&!send(peers[3],70003));assert(!endpoint.free());
  for(unsigned i:peers)assert(endpoint.hasPrivatePeerWork(r.domain.peers[i].binding.node));
  const auto programs=r.domain.nor.programs,calls=r.entropy.calls,encrypts=r.backend.encrypts;
  assert(send(3,70004)==-ENOBUFS&&runtime.ready(r.domain.local.binding.node));
  // Queue fullness alone could mask an illicit eviction. Exact pending retries
  // must still hit all FOUR original child sessions without re-encryption.
  for(unsigned i=0;i<4;++i)assert(!send(peers[i],70000+i,i?0:1));
  assert(r.domain.nor.programs==programs&&r.entropy.calls==calls&&r.backend.encrypts==encrypts);
  assert(active->state==mt::Endpoint::State::Active&&waiting->state==mt::Endpoint::State::Waiting&&waiting->due==due);
  endpoint.disconnect(1);assert(active->terminal&&endpoint.hasPrivatePeerWork(r.domain.peers[0].binding.node));
  assert(send(3,70004)==-ENOBUFS&&active->frame.len==borrowed.size()&&!memcmp(active->frame.bytes,borrowed.data(),borrowed.size()));
  endpoint.complete(*active,103,0);assert(!endpoint.hasPrivatePeerWork(r.domain.peers[0].binding.node)&&endpoint.free()==1);
  assert(!send(3,70004)); // Only after native-completion modeling releases peer0.
  for(unsigned i=1;i<4;++i)assert(!send(peers[i],70000+i));
  endpoint.cancelPrivate();runtime.sweep(endpoint);assert(endpoint.idle());r.resume();
  pass("busy queued/active/waiting and terminal-borrowed sessions retained; idle slot alone admits cold peer");
}
void basicProvider(Running &r){
  PkiPeerSet all;assert(r.storage.preparedPeers(all));auto *identity=r.storage.runtimeIdentity();assert(identity);
  BasicNonces basic;MultiPeerRuntime rejected(r.crypto,r.backend,*identity,basic,r.storage);
  assert(!rejected.open(r.domain.local.binding.node,all)&&!basic.calls);
  PkiPeerPin pins[4];for(unsigned i=0;i<4;++i)pins[i]=r.domain.pin(i);
  PkiPeerSet four;assert(four.assign(r.domain.local.binding,pins,4));
  MultiPeerRuntime runtime(r.crypto,r.backend,*identity,basic,r.storage);assert(runtime.open(r.domain.local.binding.node,four));
  mt::Endpoint endpoint;const auto plain=data("basic-provider reuse guard");
  assert(!runtime.submit(endpoint,0,pins[0].node,80000,plain.data(),plain.size(),0,false,nullptr,0,true,100));
  auto *job=endpoint.next(100);assert(job);endpoint.started(*job,0,100,100,0);endpoint.complete(*job,101,0);runtime.sweep(endpoint);
  const auto calls=basic.calls;assert(calls==1&&endpoint.idle());
  assert(runtime.submit(endpoint,0,pins[0].node,80000,plain.data(),plain.size(),0,false,nullptr,0,true,102)<0&&basic.calls==calls);
  assert(!runtime.submit(endpoint,0,pins[1].node,80001,plain.data(),plain.size(),0,false,nullptr,0,true,103));
  endpoint.cancelPrivate();runtime.sweep(endpoint);
  pass("nonpersistent provider refuses over-four open; four-contact session RAM reuse guard survives retire");
}
}

int main(){
  static_assert(PkiPeerSet::Limit==16&&PkiAppendHistory::ActivePeerLimit==16&&RuntimeSlots==4,"M101 separate bounded capacities");
  static_assert(PkiLedger::PeerLimit==4&&PkiLedger::RecordSize==1944,"Immutable legacy baseline wire layout");
  Domain d;const auto originalIdentity=d.nvs.values.at("mtidentity/state0");
  PkiPeerPin pins[17];for(unsigned i=0;i<17;++i)pins[i]=d.pin(i);
  PkiPeerSet set;assert(set.assign(d.local.binding,pins,16));const auto prior=set;
  assert(!set.assign(d.local.binding,pins,17)&&set.count()==16);
  for(unsigned i=0;i<16;++i){const auto *current=set.at(i),*saved=prior.at(i);
    assert(current&&saved&&current->node==saved->node&&!memcmp(current->publicKey,saved->publicKey,32));}
  pass("sixteen distinct synthetic pins accepted atomically; seventeenth leaves prior pins intact");
  std::map<std::string,Bytes> selected;Bytes root,afterTraffic;unsigned written=0;
  {
    Running r(d);r.boot(4);r.start();const auto legacy=d.nvs.values;
    assert(r.add(4)<0&&d.nvs.values==legacy&&!d.nor.programs);
    PkiLedger baseline;assert(!baseline.load(d.nvs));Bytes frozen(PkiLedger::RecordSize);
    assert(baseline.snapshotRead(0,frozen.data(),frozen.size()));r.enable();selected=d.nvs.values;
    root.assign(d.nor.bytes.begin(),d.nor.bytes.begin()+PkiAppendHistory::RootBytes);
    assert(!memcmp(root.data()+128,frozen.data(),frozen.size()));
    for(unsigned i=4;i<16;++i)assert(!r.add(i));
    assert(d.nvs.values==selected&&r.startup.loadedPeers()==4);sixteen(r);
    assert(!memcmp(root.data(),d.nor.bytes.data(),root.size()));
    PkiLedger::Status status;assert(r.storage.ledgerStatus(status)&&status.format==3&&status.active==16&&status.stored==16);
    const auto bytes=d.nor.bytes;const auto programs=d.nor.programs;
    assert(r.add(16)<0&&d.nor.programs==programs&&d.nor.bytes==bytes&&d.nvs.values==selected);
    const auto plain=data();const auto calls=r.entropy.calls;
    assert(r.service.submitPrivate(0,d.peers[4].binding.node,9000,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0&&r.entropy.calls==calls);
    assert(d.nvs.values.at("mtidentity/state0")==originalIdentity);
    pass("immutable four-peer baseline gains twelve append-only contacts; seventeenth has no writes and live startup snapshot stays four");
  }
  {
    const auto before=d.nor.bytes;const auto programs=d.nor.programs;
    Running r(d);r.boot(16);sixteen(r);assert(d.nor.bytes==before&&d.nor.programs==programs&&d.nvs.values==selected&&r.reader.begins==r.reader.ends);r.start();
    const auto plain=data("sixteen-peer sequential TX");
    for(unsigned i=0;i<16;++i){
      const uint32_t now=1000+5000*i,id=10000+i;assert(r.clock.set(now));const auto sends=r.radio.sends;
      assert(!r.service.submitPrivate(0,d.peers[i].binding.node,id,plain.data(),plain.size(),0,false,nullptr,0,true,now));
      r.poll(now+500);assert(r.radio.sends==sends+1);const auto frame=r.radio.frames.back();
      assert(frame.size()>=29&&bench::le32(frame.data())==d.peers[i].binding.node&&bench::le32(frame.data()+4)==d.local.binding.node&&bench::le32(frame.data()+8)==id);
      uint8_t decoded[227]{};size_t size=0;
      assert(r.crypto.decrypt(d.peers[i].scalar,d.local.binding.publicKey,d.local.binding.node,id,frame.data()+16,frame.size()-16,decoded,sizeof(decoded),size));
      assert(size==plain.size()&&!memcmp(decoded,plain.data(),size));
      r.radio.done=true;r.poll(now+501);mt::Endpoint::Result result;
      assert(r.service.takeMessageResult(0,result)&&result.id==id&&result.kind==mt::Endpoint::Kind::Local&&!result.code);
      r.receive(i,20000+i,now+502,true);const auto writes=d.nor.programs;
      r.receive(i,20000+i,now+503,false);assert(d.nor.programs==writes&&r.storage.seen(d.peers[i].binding.node,d.peers[i].binding.publicKey,20000+i)==ReplayCheck::Seen);
    }
    assert(d.nvs.values==selected&&!memcmp(root.data(),d.nor.bytes.data(),root.size()));
    pass("actual Service TX decrypts and AEAD RX/replay refusal work sequentially across all sixteen cold/hot contacts");
    busyCache(r);basicProvider(r);afterTraffic=d.nor.bytes;written=d.nor.programs;
  }
  {
    Running r(d);r.boot(16);r.start();sixteen(r);
    assert(d.nor.bytes==afterTraffic&&d.nor.programs==written&&d.nvs.values==selected&&d.nvs.values.at("mtidentity/state0")==originalIdentity);
    const auto plain=data("durable ID reuse");const auto calls=r.entropy.calls;
    for(unsigned i=0;i<16;++i){
      assert(r.service.submitPrivate(0,d.peers[i].binding.node,10000+i,plain.data(),plain.size(),0,false,nullptr,0,true,100+i)<0);
      r.receive(i,20000+i,200+i,false);
    }
    assert(!r.radio.sends&&d.nor.programs==written&&d.nor.bytes==afterTraffic&&r.entropy.calls==calls&&d.nvs.values==selected);
    pass("reboot keeps all sixteen durable nonce and authenticated replay histories without reset, erase or NVS rewrite");
  }
  {
    Domain revoked;
    {Running r(revoked);r.boot(4);r.start();r.enable();for(unsigned i=4;i<16;++i)assert(!r.add(i));}
    const auto nvs=revoked.nvs.values;
    {Running r(revoked);r.boot(16);r.start();
      // Boot leaves contacts0/1/2/15 hot; contact3 is deliberately cold.
      for(unsigned peer:{3u,0u,1u,2u,15u}){
        assert(r.service.revokePrivatePeer(revoked.peers[peer].binding.node));
        assert(r.storage.revoked(revoked.peers[peer].binding.node)&&r.service.receiving()&&r.service.endpointArmed()&&!r.service.faulted());
        const auto writes=revoked.nor.programs,calls=r.entropy.calls;const auto plain=data();
        assert(r.service.submitPrivate(0,revoked.peers[peer].binding.node,91000+peer,plain.data(),plain.size(),0,false,nullptr,0,true,100)<0);
        assert(revoked.nor.programs==writes&&r.entropy.calls==calls);
      }
      // Revoking the final original hot child must seed a surviving cold pin.
      r.receive(4,92000,1000,true);const auto plain=data("survivor after last-hot revoke");
      assert(!r.service.submitPrivate(0,revoked.peers[4].binding.node,92001,plain.data(),plain.size(),0,false,nullptr,0,true,1100));
      r.poll(1600);assert(r.radio.sends==1);r.radio.done=true;r.poll(1601);
      mt::Endpoint::Result result;assert(r.service.takeMessageResult(0,result)&&result.id==92001&&!result.code);
      pass("cold revocation and final-hot revocation retain a surviving cold peer without global hold");
      for(unsigned peer=4;peer<15;++peer)assert(r.service.revokePrivatePeer(revoked.peers[peer].binding.node));
      assert(r.service.privateDormant()&&r.service.receiving()&&r.service.endpointArmed()&&
        !r.service.faulted()&&revoked.nvs.values==nvs);
    }
    {const auto before=revoked.nor.bytes;const auto writes=revoked.nor.programs;
      Running r(revoked);r.boot(0);assert(r.startup.state()==PkiStartup::State::Dormant);
      assert(r.service.privateConfigured()&&r.service.privateDormant()&&r.storage.runtimeIdentity());
      assert(r.service.start(false,0)&&r.service.armEndpoint(true)&&r.service.receiving()&&!r.service.faulted());
      const auto entropyCalls=r.entropy.calls;const auto plain=data("revoked contact remains private-denied");
      assert(r.service.submitPrivate(0,revoked.peers[15].binding.node,93000,plain.data(),plain.size(),0,false,nullptr,0,true,1200)<0);
      assert(!r.radio.sends&&r.entropy.calls==entropyCalls&&revoked.nor.programs==writes&&revoked.nor.bytes==before);
      assert(revoked.nor.programs==writes&&revoked.nor.bytes==before&&revoked.nvs.values==nvs);
      pass("all sixteen tombstones reconstruct public-ready and dormant without private TX or nonce, root or identity reset");}
  }
  std::cout<<"contact cache suite: "<<groups<<" groups\n";
}
