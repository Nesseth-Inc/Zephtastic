// SPDX-License-Identifier: GPL-3.0-only
// Synthetic credentials and memory-only stores; no device or key export.
#include "pki_runtime_fixture.h"
#include "pki/PkiPeerAdministration.h"
#include "pki/PkiStorage.h"

struct AdminMemory:Memory {
  bool badRead=false;
  int read(const char *name,uint8_t *out,size_t cap)override{
    const int n=Memory::read(name,out,cap);
    return badRead&&n>0&&!strncmp(name,"mtpki/",6)?n-1:n;
  }
};
struct AdminBoot:BootSettings {
  AdminMemory &memory;bool held=false;
  explicit AdminBoot(AdminMemory &m):memory(m){}
  int begin()override{if(held)return -EBUSY;held=true;return 0;}
  void end()override{held=false;}
  int read(const char *name,uint8_t *out,size_t cap)override{return held?memory.read(name,out,cap):-EACCES;}
};
struct AdminStorageContext:PkiStorageContext {
  unsigned depth=0;bool boot=true,paused=false;
  bool tryEnter()override{++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool bootQuiescent()const override{return depth&&boot;}
  bool quiescent()const override{return depth&&paused;}
};
struct AdminContext:PeerAdministrationContext {
  AdminStorageContext &context;PkiStorage &storage;
  bool connected=true,permit=true,allowPeer=true,allowLock=true,allowPause=true,allowResume=true,held=false;
  uint32_t epoch=17;unsigned pauses=0,resumes=0,holds=0;
  std::function<void()> onPause,onResume;
  AdminContext(AdminStorageContext &c,PkiStorage &s):context(c),storage(s){}
  bool tryEnter()override{return allowLock&&context.tryEnter();}
  void leave()override{context.leave();}
  bool current(uint32_t e)const override{return connected&&e==epoch;}
  bool ready()const override{return permit&&!held;}
  bool peerAllowed(const IdentityBinding &,const PkiPeerPin &)const override{return allowPeer;}
  bool pause()override{
    if(!allowPause||context.paused)return false;
    ++pauses;context.paused=true;if(onPause)onPause();return true;
  }
  bool resume()override{
    ++resumes;if(!allowResume)return false;
    context.paused=false;if(onResume)onResume();return true;
  }
  void hold()override{++holds;held=true;context.paused=false;assert(storage.close());}
};
struct AdminFixture {
  Key local{11},stock{97},second{149},third{197},fourth{233},extra{43};
  AdminMemory memory;AdminBoot boot{memory};AdminStorageContext context;Entropy entropy;
  PkiStorage storage{boot,memory,context,entropy};AdminContext admission{context,storage};
  PkiPeerAdministration admin{storage,admission};KeyRef ref;Bytes identityBefore;
  AdminFixture(){
    IdentityJournal identity;PkiLedger ledger;MonocypherIdentityDeriver derive;
    assert(!identity.load(memory,derive));IdentityCandidate c(local.binding,local.scalar);
    assert(!identity.store(memory,derive,c,true));assert(!ledger.load(memory));
    assert(!ledger.initialize(memory,local.binding.publicKey,local.binding.node,true));
    assert(!ledger.addTrustedPeer(memory,stock.binding.node,stock.binding.publicKey,true));
    assert(!storage.prepare()&&!storage.activate(local.binding.node));
    assert(context.tryEnter());IdentityBinding b;assert(storage.runtimeIdentity()->binding(b,ref));context.leave();
    context.boot=false;context.paused=true;uint32_t nonce=0;
    assert(storage.reserve(ref,local.binding.publicKey,local.binding.node,111,nonce));
    assert(storage.commitAuthenticated(stock.binding.node,stock.binding.publicKey,222));context.paused=false;
    identityBefore=memory.values.at("mtidentity/state0");
  }
  UsbPeerCommand command(const Key &peer)const{
    UsbPeerCommand c;c.localNode=local.binding.node;memcpy(c.localPublic,local.binding.publicKey,32);
    c.peerNode=peer.binding.node;memcpy(c.peerPublic,peer.binding.publicKey,32);return c;
  }
  void preserved(){
    assert(memory.values.at("mtidentity/state0")==identityBefore);
    assert(storage.active(stock.binding.node,stock.binding.publicKey));
    assert(storage.seen(stock.binding.node,stock.binding.publicKey,222)==ReplayCheck::Seen);
    uint32_t nonce=99;context.paused=true;const auto writes=memory.writes;
    assert(!storage.reserve(ref,local.binding.publicKey,local.binding.node,111,nonce));
    assert(nonce==99&&writes==memory.writes);context.paused=false;
  }
  size_t reloadCount(){
    memory.fault=0;memory.badRead=false;memory.onWrite={};
    AdminBoot reader(memory);AdminStorageContext nextContext;Entropy nextEntropy;
    PkiStorage next(reader,memory,nextContext,nextEntropy);assert(!next.prepare());
    IdentityBinding b;PkiPeerSet pins;assert(next.preparedBinding(b)&&b.node==local.binding.node);
    assert(!memcmp(b.publicKey,local.binding.publicKey,32)&&next.preparedPeers(pins));
    assert(!next.activate(local.binding.node));assert(next.seen(stock.binding.node,stock.binding.publicKey,222)==ReplayCheck::Seen);
    uint32_t nonce=99;nextContext.paused=true;KeyRef r;assert(nextContext.tryEnter());
    assert(next.runtimeIdentity()->binding(b,r));nextContext.leave();
    assert(!next.reserve(r,b.publicKey,b.node,111,nonce)&&nonce==99);return pins.count();
  }
};
static unsigned groups=0;
static void pass(const char *s){++groups;std::cout<<"PASS "<<s<<'\n';}
int main(){
  {
    AdminFixture f;const auto writes=f.memory.writes,calls=f.entropy.calls;
    assert(!f.admin.add(f.command(f.second),17));PkiPeerSet pins;assert(f.storage.preparedPeers(pins)&&pins.count()==2);
    assert(pins.find(f.stock.binding.node)&&pins.find(f.second.binding.node));
    assert(f.memory.writes==writes+1&&f.entropy.calls==calls&&f.admission.pauses==1&&f.admission.resumes==1&&!f.admission.holds);
    f.preserved();assert(f.reloadCount()==2);
    pass("peer-only append keeps identity, stock pin, old nonce/replay and reloads both peers without entropy");
  }
  for(unsigned mode=0;mode<8;++mode){
    AdminFixture f;auto command=f.command(f.second);const auto rows=f.memory.values;const auto writes=f.memory.writes;
    if(mode==0)command.localNode^=1;
    if(mode==1)command.localPublic[0]^=1;
    if(mode==2)command.peerNode^=1;
    if(mode==3)memset(command.peerPublic,0,32);
    if(mode==4)command=f.command(f.local);
    if(mode==5)f.admission.allowPeer=false;
    if(mode==6)command.peerNode=0;
    if(mode==7){command.peerNode=f.local.binding.node;memcpy(command.peerPublic,f.local.binding.publicKey,32);}
    assert(f.admin.add(command,17)==-EINVAL&&f.memory.values==rows&&f.memory.writes==writes);
    assert(!f.admission.pauses&&!f.context.depth);
  }
  pass("exact local binding, curve/CRC/self/node and collision policy refuse before pause/write");
  for(unsigned mode=0;mode<5;++mode){
    AdminFixture f;const auto writes=f.memory.writes;
    if(mode==0)f.admission.allowLock=false;
    if(mode==1)f.admission.connected=false;
    if(mode==2)f.admission.permit=false;
    if(mode==3)f.admission.allowPause=false;
    const int rc=f.admin.add(f.command(f.second),mode==4?18:17);
    assert(rc==(mode==0||mode==3?-EBUSY:-EACCES));
    assert(f.memory.writes==writes&&!f.admission.holds&&!f.context.depth);
  }
  pass("lock, measured epoch, ready and native-pause refusal do not persist or change ownership");
  {
    AdminFixture f;const auto writes=f.memory.writes;f.admission.onPause=[&]{f.admission.connected=false;};
    assert(f.admin.add(f.command(f.second),17)==-EACCES&&f.memory.writes==writes);
    assert(f.admission.resumes==1&&!f.admission.holds&&!f.context.paused);f.preserved();
    pass("disconnect before write restores without new trust");
  }
  for(unsigned mode=0;mode<3;++mode){
    AdminFixture f;const auto writes=f.memory.writes;
    f.memory.onWrite=[&]{assert(f.context.paused&&f.context.depth);if(mode==2)f.memory.badRead=true;};
    f.memory.fault=mode==0?1:mode==1?2:0;
    assert(f.admin.add(f.command(f.second),17)==-EIO&&f.admission.holds==1&&f.memory.writes==writes+1);
    const auto after=f.memory.writes;assert(f.admin.add(f.command(f.second),17)==-EACCES&&after==f.memory.writes);
    assert(f.reloadCount()==(mode==0?1:2));
  }
  pass("before/after-write and exact-readback uncertainty hold without retry; reload observes exact old/new pin set");
  for(unsigned mode=0;mode<3;++mode){
    AdminFixture f;const auto writes=f.memory.writes;
    if(mode==0)f.memory.onWrite=[&]{f.admission.connected=false;};
    if(mode==1)f.admission.allowResume=false;
    if(mode==2)f.admission.onResume=[&]{f.admission.connected=false;};
    assert(f.admin.add(f.command(f.second),17)<0&&f.memory.writes==writes+1&&f.admission.holds==1);
    assert(f.reloadCount()==2);
  }
  pass("epoch loss during/after commit and restore failure hold while committed trust stays durable");
  {
    AdminFixture f;const auto command=f.command(f.second);const auto writes=f.memory.writes;
    f.memory.onWrite=[&]{assert(f.admin.add(f.command(f.third),17)==-EBUSY);};
    assert(!f.admin.add(command,17)&&f.memory.writes==writes+1&&!f.context.depth);
    f.preserved();pass("reentrant administration cannot gain another write or leak service lease");
  }
  {
    AdminFixture f;const auto first=f.memory.writes;
    assert(f.admin.add(f.command(f.stock),17)==-EEXIST&&f.memory.writes==first);
    for(auto *key:{&f.second,&f.third,&f.fourth})assert(!f.admin.add(f.command(*key),17));
    const auto full=f.memory.writes;assert(full==first+3);
    assert(f.admin.add(f.command(f.extra),17)==-ENOSPC&&f.memory.writes==full);
    assert(f.admin.add(f.command(f.second),17)==-EEXIST&&f.memory.writes==full);
    f.preserved();assert(f.reloadCount()==4);pass("four total pins reload; duplicates and fifth pin refuse without replacing identity/history");
  }
  std::cout<<"{\"PASS\":true,\"groups\":"<<groups<<",\"hardware\":false,\"synthetic_credentials_only\":true}\n";
}
