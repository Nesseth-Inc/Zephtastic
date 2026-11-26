// SPDX-License-Identifier: GPL-3.0-only
// Uncompiled integration candidate. Deterministic credentials, modeled NOR/NVS
// and real Service pause/PSA APIs; no target flash or hardware proof. Bulk RX
// commits exercise the authenticated-authority seam; separate RX cases use AEAD.
#include "pki_runtime_fixture.h"
#include "known_channel_fixture.h"
#include "pki/PkiAppendHistory.h"
#include "pki/PkiStartup.h"
#include "pki/PkiPeerAdministration.h"
#include "NodeAdmin.h"

namespace {
struct Nor final:AppendStore {
  Bytes bytes=Bytes(PkiAppendHistory::StoreBytes,0xff);
  unsigned programs=0,reads=0;unsigned fault=0;bool readFault=false;
  std::function<void()> programGuard;
  size_t capacity()const override{return bytes.size();}
  int read(size_t offset,uint8_t *out,size_t size)override{
    ++reads;if(readFault)return -EIO;
    if(!out||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    memcpy(out,bytes.data()+offset,size);return 0;
  }
  int program(size_t offset,const uint8_t *data,size_t size)override{
    assert(programGuard);programGuard();++programs;
    if(!data||!size||(offset&3)||(size&3)||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    if(fault==1)return -EIO;
    for(size_t i=0;i<size;++i)if((bytes[offset+i]&data[i])!=data[i])return -EIO;
    const size_t written=(fault==2||fault>=4)?(size>4?size/2:1):size;
    for(size_t i=0;i<written;++i)bytes[offset+i]&=data[i];
    if(fault==4)return -EEXIST; // Partial program errno never proves a no-write refusal.
    if(fault==5)return -ENOTEMPTY;
    if(fault==6)return -EAGAIN;
    return fault?-EIO:0; // fault3 models full programming with uncertain status.
  }
};
struct Domain {
  Key local{11},peers[5]{Key{97},Key{61},Key{149},Key{193},Key{227}};
  Memory nvs;Nor nor;
  explicit Domain(unsigned count=2){
    assert(count>=1&&count<=4);MonocypherIdentityDeriver derive;IdentityJournal identity;PkiLedger ledger;
    assert(!identity.load(nvs,derive));IdentityCandidate candidate(local.binding,local.scalar);
    assert(!identity.store(nvs,derive,candidate,true)&&!ledger.load(nvs));
    assert(!ledger.initialize(nvs,local.binding.publicKey,local.binding.node,true));
    for(unsigned i=0;i<count;++i)assert(!ledger.addTrustedPeer(nvs,peers[i].binding.node,peers[i].binding.publicKey,true));
    assert(!ledger.reserve(nvs,51,true));
    for(unsigned i=0;i<count;++i)assert(!ledger.commitAuthenticated(nvs,peers[i].binding.node,peers[i].binding.publicKey,61+i,true));
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
  mt::test::KnownChannelFixtureClock knownChannelClock;
  mt::Service service{radio,true,0,127,true,false};
  explicit Running(Domain &d):domain(d),reader(d.nvs),append(d.nor),
    storage(reader,d.nvs,context,entropy,&append),startup(storage,crypto,backend,signing){
    knownChannelClock.bind(service);
    context.service=&service;mt::Config config;config.bytes[4]=1;auto node=config.node();node.setPublic();config.node(node);
    assert(service.apply(config)&&service.requirePrivateRuntime());
    domain.nor.programGuard=[this]{assert(service.privateStoragePaused()&&radio.healthValue==0);};
    domain.nvs.onWrite=[this]{assert(service.privateStoragePaused()&&radio.healthValue==0);};
  }
  ~Running(){
    // End the old Service/runtime/storage ownership scope before reconstruction.
    service.stop();assert(storage.close());domain.nor.programGuard={};domain.nvs.onWrite={};
  }
  bool boot(){
    uint32_t node=0;
    if(!startup.prepare(node)){startup.hold(service);return false;}
    assert(node==domain.local.binding.node);service.route.configureContinuous(uint8_t(node),8);
    service.route.configureIdentity(node);service.discovery.identity(node);
    return startup.activate(service,true);
  }
  void start(){assert(service.start(false,0)&&service.armEndpoint(true));}
  void pause(){assert(service.beginPrivateAdministration()&&service.privateStoragePaused()&&!radio.healthValue);}
  void resume(){assert(service.endPrivateAdministration());}
  IdentityBinding binding(){IdentityBinding b;assert(storage.preparedBinding(b));return b;}
  KeyRef key(){
    auto *identity=storage.runtimeIdentity();IdentityBinding b;KeyRef ref;
    assert(identity&&context.tryEnter()&&identity->binding(b,ref));context.leave();return ref;
  }
  void enable(){const auto b=binding();pause();assert(!storage.enableLongStorage(b));resume();assert(storage.extendedStorage());}
  int add(unsigned i){
    const auto b=binding();PkiPeerPin pin;pin.node=domain.peers[i].binding.node;memcpy(pin.publicKey,domain.peers[i].binding.publicKey,32);
    pause();const int result=storage.addTrustedPeer(b,pin);resume();return result;
  }
  bool reserve(uint32_t id,uint32_t &extra){
    const auto b=binding();const auto ref=key();pause();const bool result=storage.reserve(ref,b.publicKey,b.node,id,extra);
    if(storage.state()==PkiStorage::State::Active)resume();else startup.hold(service);
    return result;
  }
  bool commit(unsigned peer,uint32_t id){
    pause();const bool result=storage.commitAuthenticated(domain.peers[peer].binding.node,domain.peers[peer].binding.publicKey,id);
    if(storage.state()==PkiStorage::State::Active)resume();else startup.hold(service);
    return result;
  }
  Bytes incoming(unsigned peer,uint32_t id){
    const auto plain=data("long-storage synthetic AEAD");uint8_t ciphertext[239]{};size_t n=0;
    const auto &sender=domain.peers[peer];
    assert(crypto.encrypt(sender.scalar,domain.local.binding.publicKey,sender.binding.node,id,0x12345678,
      plain.data(),plain.size(),ciphertext,sizeof(ciphertext),n));
    Bytes frame(16+n);put(frame.data(),domain.local.binding.node);put(frame.data()+4,sender.binding.node);put(frame.data()+8,id);
    frame[15]=uint8_t(sender.binding.node);memcpy(frame.data()+16,ciphertext,n);return frame;
  }
  void receive(unsigned peer,uint32_t id){
    auto frame=incoming(peer,id);service.rx(frame.data(),frame.size(),3,1500,71);PrivateEvent event;
    for(uint8_t owner:{0,1})assert(service.takePrivateEvent(owner,frame.data(),frame.size(),event)&&
      event.from==domain.peers[peer].binding.node&&event.id==id);
    assert(!service.takePrivateEvent(0,frame.data(),frame.size(),event));
  }
};
unsigned groups=0;
void pass(const char *s){++groups;std::cout<<"PASS "<<s<<'\n';}
void state(Running &r,unsigned sent,unsigned received,unsigned stored,unsigned active){
  PkiLedger::Status s;assert(r.storage.ledgerStatus(s)&&s.format==3&&s.sent==sent&&s.received==received&&s.stored==stored&&s.active==active);
}
struct EraseSpy final:HistoryPageEraser {
  unsigned calls=0,validations=0;
  int validate()const override{return 0;}
  int erasePage(unsigned)override{++calls;return -EACCES;}
};
struct PreparationTick final:PreparationProgress {
  unsigned calls=0;int error=0;
  int checkpoint()override{++calls;return error;}
};
struct Administration final:PeerAdministrationContext {
  Running &run;bool held=false,epoch=true,allow=true,pauseOk=true,resumeOk=true;
  unsigned holds=0,resumes=0;
  explicit Administration(Running &r):run(r){}
  bool tryEnter()override{if(held)return false;held=true;return true;}
  void leave()override{assert(held);held=false;}
  bool current(uint32_t e)const override{return epoch&&e==7;}
  bool ready()const override{return allow;}
  bool peerAllowed(const IdentityBinding &,const PkiPeerPin &)const override{return true;}
  bool pause()override{return pauseOk&&run.service.beginPrivateAdministration();}
  bool resume()override{++resumes;return resumeOk&&run.service.endPrivateAdministration();}
  void hold()override{++holds;assert(!run.context.held);run.startup.hold(run.service);}
};
}

int main(){
  static_assert(PkiAppendHistory::StoreBytes==172032,"Explicit modeled append region");
  for(unsigned mode=0;mode<12;++mode){
    Domain d;Running r(d);assert(r.boot());r.start();auto binding=r.binding();
    TailProbeResult probe;uint32_t gen=0,crc=0;
    assert(!r.storage.probeLegacyTail(d.nor,probe,gen,crc));
    const auto before=d.nvs.values;EraseSpy eraser;PreparationTick tick;TailPreparationResult out;
    if(mode!=0)r.pause();
    if(mode==1)++binding.version;
    if(mode==2)++binding.algorithm;
    if(mode==3)++binding.generation;
    if(mode==4)++binding.node;
    if(mode==5)binding.publicKey[0]^=1;
    if(mode==6)++gen;
    if(mode==7)crc^=1;
    if(mode==8)d.nor.bytes[0]=0;
    if(mode==9)memcpy(d.nor.bytes.data(),"ZTAH",4);
    if(mode==10)d.nor.readFault=true;
    if(mode==11)tick.error=-ETIMEDOUT;
    assert(r.storage.prepareFormerCode(d.nor,eraser,tick,binding,gen,crc,out)<0);
    assert(!eraser.calls&&!out.attempted&&!out.verified&&!out.complete&&d.nvs.values==before&&!d.nor.programs);
    d.nor.readFault=false;if(mode!=0)r.resume();
  }
  pass("former-code preparation refuses wrong full identity, legacy proof, pause, fingerprint, journal and progress without writes");
  for(unsigned fault=0;fault<5;++fault){
    Domain d;Running r(d);assert(r.boot());r.start();const auto binding=r.binding();
    TailProbeResult probe;uint32_t gen=0,crc=0;
    assert(!r.storage.probeLegacyTail(d.nor,probe,gen,crc));const auto before=d.nvs.values;
    UsbTailPrepareCommand command;command.localNode=binding.node;memcpy(command.localPublic,binding.publicKey,32);command.generation=gen;command.crc=crc;
    Administration context(r);PkiPeerAdministration admin(r.storage,context);EraseSpy eraser;PreparationTick tick;TailPreparationResult out;
    if(fault==1)context.epoch=false;
    if(fault==2)context.pauseOk=false;
    if(fault==3)context.resumeOk=false;
    if(fault==4)tick.error=-ETIMEDOUT;
    const int rc=admin.prepareTail(d.nor,eraser,tick,command,7,out);
    assert(!eraser.calls&&!out.attempted&&d.nvs.values==before&&!d.nor.programs&&!context.held);
    TailPreparationResult report;int error=0;bool observed=false;
    assert(r.storage.preparationReport(report,error,observed));
    if(!fault){assert(!rc&&out.blank&&out.complete&&!context.holds&&context.resumes==1&&r.service.receiving());}
    else {assert(rc<0&&!out.complete);if(fault!=1)assert(context.holds==1&&r.startup.state()==PkiStartup::State::Held);}
    if(fault==0)assert(observed&&!error&&report.complete&&report.blank);
    if(fault==3||fault==4)assert(observed&&error==rc&&!report.complete);
    if(fault==1||fault==2)assert(!observed&&error==-ENOENT);
  }
  pass("already blank performs zero erases; native pause/restore failures hold only after storage Lease release");
  {
    Domain d;Running r(d);assert(r.boot());r.start();const auto binding=r.binding();r.enable();const auto before=d.nvs.values;
    r.pause();EraseSpy eraser;PreparationTick tick;TailPreparationResult out;
    assert(r.storage.prepareFormerCode(d.nor,eraser,tick,binding,1,1,out)<0&&!eraser.calls);
    assert(d.nvs.values==before);r.resume();
    pass("selected extended history never admits former-code preparation");
  }
  {
    Domain d;Running r(d);TailProbeResult out;out.nonFF=7;out.digest[0]=91;
    uint32_t generation=0x11223344,crc=0x55667788;
    auto unchanged=[&]{assert(out.nonFF==7&&out.digest[0]==91&&!out.blank&&!out.possibleJournal&&
      generation==0x11223344&&crc==0x55667788);};
    assert(r.storage.probeLegacyTail(d.nor,out,generation,crc)==-EACCES);unchanged();
    assert(r.boot());r.start();const auto before=d.nvs.values;
    const auto reads=d.nor.reads;
    r.context.held=true;assert(r.storage.probeLegacyTail(d.nor,out,generation,crc)==-EBUSY);
    r.context.held=false;unchanged();assert(d.nor.reads==reads);
    d.nor.readFault=true;assert(r.storage.probeLegacyTail(d.nor,out,generation,crc)==-EIO);
    unchanged();d.nor.readFault=false;
    const auto begin=d.nor.reads;
    assert(!r.storage.probeLegacyTail(d.nor,out,generation,crc)&&out.blank&&!out.nonFF&&!out.possibleJournal);
    uint8_t expected[32]{};crypto_blake2b(expected,32,d.nor.bytes.data(),d.nor.bytes.size());
    assert(!memcmp(out.digest,expected,32)&&d.nor.reads-begin==2688&&generation==6);
    // The source-owned normalized baseline CRC, not an exported NVS record.
    bool matched=false;
    for(const auto &entry:before)if(entry.first.find("mtpki/ledger")==0&&
      bench::le32(entry.second.data()+8)==generation)
      matched=crc==bench::le32(entry.second.data()+PkiLedger::RecordSize-4);
    assert(matched&&before==d.nvs.values&&!d.nor.programs&&r.service.receiving()&&r.service.endpointArmed());
    r.enable();const auto selected=d.nvs.values;const auto count=d.nor.reads;
    out=TailProbeResult{};out.nonFF=7;out.digest[0]=91;generation=0x11223344;crc=0x55667788;
    assert(r.storage.probeLegacyTail(d.nor,out,generation,crc)==-EACCES);unchanged();
    assert(d.nor.reads==count&&d.nvs.values==selected&&r.storage.extendedStorage());
    pass("read-only legacy tail proof preserves state, rejects busy/inactive/selected authority and leaves failed outputs unchanged");
  }

  {
    Domain d;Running r(d);assert(r.boot());r.start();const auto identity=d.nvs.values.at("mtidentity/state0");
    // Only the selector write is permitted after root payload+seal+CMIT readback.
    unsigned selectorWrites=0;d.nvs.onWrite=[&]{
      assert(r.service.privateStoragePaused()&&!r.radio.healthValue);
      assert(bench::le32(d.nor.bytes.data()+4092)==0x54494d43u);
      assert(!memcmp(d.nor.bytes.data(),"ZTAH",4)&&d.nor.programs>0);++selectorWrites;
    };
    const auto before=d.nvs.values;r.enable();assert(selectorWrites==1);
    unsigned unchanged=0,selected=0;
    for(const auto &entry:before)if(entry.first.find("mtpki/ledger")==0){
      if(d.nvs.values.at(entry.first)==entry.second)++unchanged;
      else {const auto &value=d.nvs.values.at(entry.first);assert(!memcmp(value.data(),"ZTPA",4));++selected;}
    }
    assert(unchanged==1&&selected==1&&d.nvs.values.at("mtidentity/state0")==identity);
    assert(r.storage.seen(d.peers[0].binding.node,d.peers[0].binding.publicKey,61)==ReplayCheck::Seen);
    assert(r.storage.seen(d.peers[1].binding.node,d.peers[1].binding.publicKey,62)==ReplayCheck::Seen);
    state(r,1,2,2,2);assert(r.service.receiving()&&r.service.endpointArmed());
    pass("explicit root then selector-LAST activation preserves local identity and exact legacy histories");
  }
  {
    Domain d;Running r(d);assert(r.boot());r.start();const auto nvs=d.nvs.values;const auto flash=d.nor.bytes;
    auto expected=r.binding();assert(r.storage.enableLongStorage(expected)==-EACCES);
    r.pause();auto wrong=expected;wrong.node^=1;
    assert(r.storage.enableLongStorage(wrong)==-EINVAL);wrong=expected;wrong.publicKey[0]^=1;
    assert(r.storage.enableLongStorage(wrong)==-EINVAL);r.resume();
    assert(!r.storage.extendedStorage()&&d.nvs.values==nvs&&d.nor.bytes==flash&&!d.nor.programs);
    pass("no quiescence or wrong exact local binding cannot program a root or install a selector");
  }
  {
    Domain d;d.nor.bytes.back()=0xfe;Running r(d);assert(r.boot());r.start();
    const auto nvs=d.nvs.values;const auto flash=d.nor.bytes;const auto b=r.binding();r.pause();
    assert(r.storage.enableLongStorage(b)==-ENOTEMPTY&&!r.storage.extendedStorage());
    assert(d.nvs.values==nvs&&d.nor.bytes==flash&&!d.nor.programs);
    assert(r.storage.state()==PkiStorage::State::Active);r.resume();
    assert(r.service.receiving()&&r.service.endpointArmed()&&!r.service.faulted());
    pass("initial nonblank refusal preserves legacy authority without program selector or new authority");
  }
  {
    for(unsigned fault:{4u,5u,6u}){
      Domain d;Running r(d);assert(r.boot());r.start();const auto nvs=d.nvs.values;
      const auto b=r.binding();r.pause();d.nor.fault=fault;
      const int expected=fault==4?-EEXIST:fault==5?-ENOTEMPTY:-EAGAIN;
      assert(r.storage.enableLongStorage(b)==expected&&r.storage.state()!=PkiStorage::State::Active);
      assert(d.nor.programs==1&&d.nvs.values==nvs&&!r.storage.runtimeIdentity());
      r.startup.hold(r.service);assert(!r.service.start(false,0)&&!r.service.armEndpoint(true));
    }
    pass("partial root program errno EEXIST ENOTEMPTY or EAGAIN cannot take the proven no-write exemption");
  }
  {
    Domain d;std::map<std::string,Bytes> selected;Bytes history;unsigned programs=0;
    {
      Running r(d);assert(r.boot());r.start();r.enable();selected=d.nvs.values;
      for(unsigned i=0;i<200;++i){uint32_t extra=0;assert(r.reserve(10000+i,extra)&&extra);}
      for(unsigned peer=0;peer<2;++peer)for(unsigned i=0;i<100;++i)
        assert(r.commit(peer,20000+i));
      state(r,201,202,2,2);assert(d.nvs.values==selected);
      uint32_t extra=99;const auto calls=r.entropy.calls;
      assert(!r.reserve(51,extra)&&extra==99&&!r.reserve(10000,extra)&&extra==99&&r.entropy.calls==calls);
      assert(!r.commit(0,61)&&!r.commit(1,20000));
      assert(r.service.receiving()&&r.service.endpointArmed()&&!r.radio.sends);
      history=d.nor.bytes;programs=d.nor.programs;
    }
    {
      Running r(d);assert(r.boot()&&r.startup.loadedPeers()==2&&r.storage.extendedStorage());
      assert(d.nvs.values==selected&&d.nor.bytes==history&&d.nor.programs==programs&&r.reader.begins==r.reader.ends);
      state(r,201,202,2,2);for(unsigned peer=0;peer<2;++peer)for(unsigned i=0;i<100;++i)
        assert(r.storage.seen(d.peers[peer].binding.node,d.peers[peer].binding.publicKey,20000+i)==ReplayCheck::Seen);
      uint32_t extra=99;assert(!r.reserve(10199,extra)&&extra==99&&!r.commit(0,20099));
      assert(d.nvs.values==selected&&d.nor.bytes==history&&d.nor.programs==programs&&!r.entropy.calls&&!r.signing.calls&&!r.radio.sends);
    }
    pass("200 new TX and two times100 RX exceed legacy caps, survive read-only reconstruction, and never rewrite selected NVS");
  }
  {
    Domain d(4);std::map<std::string,Bytes> selected;
    {
      Running r(d);assert(r.boot());r.start();r.enable();selected=d.nvs.values;
      assert(r.service.revokePrivatePeer(d.peers[0].binding.node));
      assert(r.service.receiving()&&r.service.endpointArmed()&&!r.service.faulted()&&r.storage.revoked(d.peers[0].binding.node));
      r.receive(1,40000);const auto plain=data("surviving peer");
      assert(!r.service.submitPrivate(0,d.peers[1].binding.node,40001,plain.data(),plain.size(),0,false,nullptr,0,true,1600));
      r.service.poll(2000,73);assert(r.radio.sends==1);r.radio.done=true;r.service.poll(2001,79);
      mt::Endpoint::Result result;while(r.service.takeMessageResult(0,result)){}
      assert(r.service.receiving()&&r.service.endpointArmed()&&r.storage.seen(d.peers[1].binding.node,d.peers[1].binding.publicKey,40000)==ReplayCheck::Seen);
      assert(!r.add(4));state(r,2,5,5,4);
      const auto writes=d.nor.programs,calls=r.entropy.calls;
      assert(r.service.submitPrivate(0,d.peers[4].binding.node,40002,plain.data(),plain.size(),0,false,nullptr,0,true,2100)<0);
      assert(d.nor.programs==writes&&r.entropy.calls==calls&&d.nvs.values==selected);
    }
    {
      Running r(d);assert(r.boot()&&r.startup.loadedPeers()==4&&r.storage.revoked(d.peers[0].binding.node));
      assert(r.storage.active(d.peers[4].binding.node,d.peers[4].binding.publicKey)&&d.nvs.values==selected);
      r.start();const auto plain=data("new peer after reboot");
      assert(!r.service.submitPrivate(0,d.peers[4].binding.node,40003,plain.data(),plain.size(),0,false,nullptr,0,true,100));
    }
    pass("selective revoke keeps other RX arm and AEAD session; fifth lifetime pin takes freed active slot only after reboot");
  }
  {
    Domain d(4);
    {
      Running r(d);assert(r.boot());r.start();r.enable();assert(r.service.revokePrivatePeer(d.peers[0].binding.node)&&!r.add(4));
      assert(r.service.revokePrivatePeer(d.peers[4].binding.node)&&r.storage.revoked(d.peers[4].binding.node));
      assert(r.service.receiving()&&r.service.endpointArmed()&&!r.service.faulted());
      const auto programs=d.nor.programs;assert(r.add(0)==-EEXIST&&r.add(4)==-EEXIST&&d.nor.programs==programs);
      auto expected=r.binding();PkiPeerPin changed;changed.node=d.peers[0].binding.node^1u;
      memcpy(changed.publicKey,d.peers[0].binding.publicKey,32);r.pause();
      assert(r.storage.addTrustedPeer(expected,changed)==-EINVAL);
      changed.node=d.peers[0].binding.node;memcpy(changed.publicKey,d.peers[4].binding.publicKey,32);
      assert(r.storage.addTrustedPeer(expected,changed)==-EINVAL);r.resume();assert(d.nor.programs==programs);
      state(r,1,4,5,3);
    }
    {
      Running r(d);assert(r.boot()&&r.startup.loadedPeers()==3);
      assert(r.storage.revoked(d.peers[0].binding.node)&&r.storage.revoked(d.peers[4].binding.node));
      assert(!r.storage.active(d.peers[4].binding.node,d.peers[4].binding.publicKey));
    }
    pass("pending added pin can be removed before reboot and historical node or key never becomes a fresh domain");
  }
  {
    Domain d(1);
    {
      Running r(d);assert(r.boot());r.start();r.enable();assert(r.service.revokePrivatePeer(d.peers[0].binding.node));
      assert(r.service.privateDormant()&&r.service.receiving()&&r.service.endpointArmed()&&!r.service.faulted());
      const auto writes=d.nor.programs,calls=r.entropy.calls;const auto nvs=d.nvs.values;
      const auto plain=data("revoked final peer remains private-denied");
      assert(r.service.submitPrivate(0,d.peers[0].binding.node,41000,plain.data(),plain.size(),0,false,nullptr,0,true,1500)<0);
      assert(d.nor.programs==writes&&r.entropy.calls==calls&&d.nvs.values==nvs&&!r.radio.sends);
      r.service.stop();
      assert(r.service.privateDormant()&&!r.service.receiving()&&!r.service.endpointArmed()&&!r.service.faulted());
      assert(r.service.start(false,0)&&r.service.armEndpoint(true)&&r.service.receiving()&&r.service.endpointArmed());
      assert(!r.add(1)&&r.service.privateDormant());state(r,1,1,2,1);
    }
    {
      Running r(d);assert(r.boot()&&r.startup.loadedPeers()==1&&!r.service.privateDormant());
      assert(r.storage.revoked(d.peers[0].binding.node)&&r.storage.active(d.peers[1].binding.node,d.peers[1].binding.publicKey));
    }
    pass("last-peer revoke is dormant with explicit local administration available and reboot loads a new authorized peer");
  }
  for(unsigned extended=0;extended<2;++extended){
    Domain d(1);
    Running r(d);
    assert(r.boot());
    if(extended)r.enable();
    r.start();
    mt::NodeJournal journal;
    assert(!journal.load(d.nvs,r.service.nodes,d.local.binding.node));
    assert(r.service.nodes.find(d.peers[0].binding.node)<0);
    assert(r.storage.active(d.peers[0].binding.node,d.peers[0].binding.publicKey));
    assert(r.service.receiving());
    assert(r.service.armEndpoint(false));
    assert(!mt::NodeAdmin::edit(r.service,r.radio,journal,d.nvs,d.peers[0].binding.node,mt::NodeCache::Edit::Remove,0));
    assert(r.storage.revoked(d.peers[0].binding.node));
    assert(r.service.privateDormant()&&r.service.receiving()&&!r.service.endpointArmed()&&!r.service.faulted());
    assert(r.service.nodes.find(d.peers[0].binding.node)<0);
    assert(r.radio.sends==0);
    pass("NodeAdmin Remove of a cache-absent but PKI-active peer durably retires it into a clean intentional dormant state without sends");
  }
  {
    Domain d(1);
    struct NodeSettings final:mt::SettingsBackend {
      Running &run; unsigned writes=0;
      explicit NodeSettings(Running &r):run(r){}
      static void key(const char *name){assert(!strcmp(name,"mtbase/nodes0")||!strcmp(name,"mtbase/nodes1"));}
      int read(const char *name,uint8_t *out,size_t size)override{
        key(name); return run.domain.nvs.read(name,out,size);
      }
      int write(const char *name,const uint8_t *bytes,size_t size)override{
        key(name); assert(size==mt::NodeJournal::RecordSize);
        assert(run.service.storageQuiescent()&&!run.service.privateStoragePaused()&&!run.radio.healthValue);
        auto &memory=run.domain.nvs;
        struct Restore {Memory &memory; std::function<void()> saved; ~Restore(){memory.onWrite=saved;}} restore{memory,memory.onWrite};
        memory.onWrite=[this]{assert(run.service.storageQuiescent()&&!run.service.privateStoragePaused()&&!run.radio.healthValue);};
        ++writes; return memory.write(name,bytes,size);
      }
    };
    {
      Running r(d);
      assert(r.boot());
      r.enable();
      r.start();
      NodeSettings nodesSettings(r);
      mt::NodeJournal journal;
      assert(!journal.load(nodesSettings,r.service.nodes,d.local.binding.node));
      mt::NodeCache::Entry entry;
      entry.identity(d.peers[0].binding.node,0,0);
      assert(r.service.nodes.accept(entry,d.local.binding.node,0,0)>=0);
      assert(r.service.nodes.find(d.peers[0].binding.node)>=0);
      assert(r.storage.active(d.peers[0].binding.node,d.peers[0].binding.publicKey));
      assert(r.service.receiving());
      assert(r.service.armEndpoint(false));
      assert(!mt::NodeAdmin::edit(r.service,r.radio,journal,nodesSettings,d.peers[0].binding.node,mt::NodeCache::Edit::Remove,0));
      assert(nodesSettings.writes==1);
      assert(r.storage.revoked(d.peers[0].binding.node));
      assert(r.service.nodes.find(d.peers[0].binding.node)<0);
      assert(r.service.privateDormant()&&r.service.receiving()&&!r.service.endpointArmed()&&!r.service.faulted());
      assert(r.radio.sends==0);
    }
    {
      Running r2(d);
      assert(r2.boot());
      NodeSettings nodesSettings2(r2);
      mt::NodeJournal journal2;
      assert(!journal2.load(nodesSettings2,r2.service.nodes,d.local.binding.node));
      assert(r2.storage.revoked(d.peers[0].binding.node));
      assert(!r2.storage.active(d.peers[0].binding.node,d.peers[0].binding.publicKey));
      assert(r2.service.nodes.find(d.peers[0].binding.node)<0);
      assert(r2.service.privateDormant()&&!r2.service.receiving()&&!r2.service.endpointArmed()&&!r2.service.faulted());
      pass("NodeAdmin Remove of the final active peer stays durably forgotten across a new Running reconstruction (revoked, inactive, dormant, cache-absent)");
    }
  }
  {
    Domain d(2);
    struct NodeSettings final:mt::SettingsBackend {
      Running &run;unsigned writes=0;
      explicit NodeSettings(Running &r):run(r){}
      static void key(const char *name){assert(!strcmp(name,"mtbase/nodes0")||!strcmp(name,"mtbase/nodes1"));}
      int read(const char *name,uint8_t *out,size_t size)override{
        key(name);return run.domain.nvs.read(name,out,size);
      }
      int write(const char *name,const uint8_t *bytes,size_t size)override{
        key(name);assert(size==mt::NodeJournal::RecordSize);
        assert(run.service.storageQuiescent()&&!run.service.privateStoragePaused()&&!run.radio.healthValue);
        auto &memory=run.domain.nvs;
        struct Restore {Memory &memory;std::function<void()> saved;~Restore(){memory.onWrite=saved;}} restore{memory,memory.onWrite};
        memory.onWrite=[this]{assert(run.service.storageQuiescent()&&!run.service.privateStoragePaused()&&!run.radio.healthValue);};
        ++writes;return memory.write(name,bytes,size);
      }
    };
    {
      Running r(d);
      assert(r.boot());
      r.enable();
      r.start();
      NodeSettings nodesSettings(r);
      mt::NodeJournal journal;
      assert(!journal.load(nodesSettings,r.service.nodes,d.local.binding.node));
      for(unsigned i=0;i<2;++i){
        mt::NodeCache::Entry entry;
        entry.identity(d.peers[i].binding.node,0,0);
        assert(r.service.nodes.accept(entry,d.local.binding.node,0,0)>=0);
        assert(r.service.nodes.find(d.peers[i].binding.node)>=0);
      }
      assert(r.storage.active(d.peers[0].binding.node,d.peers[0].binding.publicKey));
      assert(r.storage.active(d.peers[1].binding.node,d.peers[1].binding.publicKey));
      assert(r.service.receiving());
      assert(r.service.armEndpoint(false));
      assert(!mt::NodeAdmin::edit(r.service,r.radio,journal,nodesSettings,d.peers[0].binding.node,mt::NodeCache::Edit::Remove,0));
      assert(nodesSettings.writes==1);
      assert(r.storage.revoked(d.peers[0].binding.node));
      assert(r.service.nodes.find(d.peers[0].binding.node)<0);
      assert(r.service.nodes.find(d.peers[1].binding.node)>=0);
      assert(!r.service.privateDormant()&&r.service.receiving()&&!r.service.faulted());
      assert(r.radio.sends==0);
    }
    Running r2(d);
    assert(r2.boot());
    assert(r2.storage.extendedStorage());
    NodeSettings nodesSettings2(r2);
    assert(!r2.service.privateDormant());
    assert(r2.storage.revoked(d.peers[0].binding.node));
    assert(!r2.storage.active(d.peers[0].binding.node,d.peers[0].binding.publicKey));
    assert(r2.storage.active(d.peers[1].binding.node,d.peers[1].binding.publicKey));
    mt::NodeJournal journal2;
    assert(!journal2.load(nodesSettings2,r2.service.nodes,d.local.binding.node));
    assert(r2.service.nodes.find(d.peers[1].binding.node)>=0);
    assert(r2.service.nodes.find(d.peers[0].binding.node)<0);
    r2.start();
    assert(r2.service.receiving()&&r2.service.endpointArmed()&&!r2.service.faulted());
    assert(r2.radio.sends==0);
    pass("NodeAdmin Remove of one cache-present peer preserves the surviving peer's identity/key and NodeCache record across a new Running reconstruction");
  }
  for(unsigned rx=0;rx<2;++rx){
    Domain d;std::map<std::string,Bytes> selected;
    {
      Running r(d);assert(r.boot());r.start();r.enable();selected=d.nvs.values;d.nor.fault=2;
      if(!rx){uint32_t extra=0xcafebabe;assert(!r.reserve(50000,extra)&&extra==0xcafebabe);}
      else {
        auto frame=r.incoming(0,50001);r.service.rx(frame.data(),frame.size(),3,1500,83);PrivateEvent event;
        event.from=event.to=event.id=0x5a5a5a5au;event.size=91;
        memset(event.publicKey,0x5a,sizeof(event.publicKey));memset(event.data,0x5a,sizeof(event.data));
        const auto before=event;
        assert(!r.service.takePrivateEvent(0,frame.data(),frame.size(),event)&&
          !r.service.takePrivateEvent(1,frame.data(),frame.size(),event)&&event.from==before.from&&event.to==before.to&&
          event.id==before.id&&event.size==before.size&&!memcmp(event.publicKey,before.publicKey,sizeof(event.publicKey))&&
          !memcmp(event.data,before.data,sizeof(event.data)));
      }
      assert(r.storage.state()!=PkiStorage::State::Active&&!r.storage.runtimeIdentity());
      assert(!r.service.start(false,0)&&!r.service.armEndpoint(true)&&!r.radio.sends&&d.nvs.values==selected);
    }
    d.nor.fault=0;const auto torn=d.nor.bytes;const auto programs=d.nor.programs;
    {
      Running r(d);assert(!r.boot()&&r.startup.state()==PkiStartup::State::Held&&r.service.faulted());
      assert(!r.service.start(false,0)&&!r.service.armEndpoint(true)&&!r.storage.runtimeIdentity());
      assert(d.nvs.values==selected&&d.nor.bytes==torn&&d.nor.programs==programs&&!r.radio.sends);
    }
    pass(rx?"uncertain authenticated RX append emits no plaintext and torn selected history never falls back at boot":
      "uncertain nonce append leaves caller output untouched and torn selected history never falls back at boot");
  }
  std::cout<<"{\"PASS\":true,\"groups\":"<<groups<<",\"hardware\":false,\"synthetic_credentials_only\":true}\n";
}
