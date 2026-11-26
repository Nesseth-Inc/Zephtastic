// SPDX-License-Identifier: GPL-3.0-only
// Independent M69 capacity/GC tests using the existing actual-NVS NOR bridge.
// All identity material below is deterministic synthetic fixture data.
#include "pki/ZephyrPkiStorage.h"
#include "MtSignature.h"
#include "monocypher.h"
#include "bridge.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
using Rows=std::map<std::string,Bytes>;
static constexpr unsigned NewTx=144,NewRx=72;
static constexpr size_t NewRecord=1944;
static unsigned checks=0,groups=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
static const char *publicNames[]={"mtbase/config","mtbase/state0","mtbase/state1","mtbase/nodes0","mtbase/nodes1"};
static const size_t publicSizes[]={80,128,128,1492,1492};
static const char *ledgerNames[]={"mtpki/ledger0","mtpki/ledger1"};
struct BackendWriteObservation {std::string key;size_t bytes=0;int result=0;unsigned attempts=0;};
static BackendWriteObservation lastBackendWrite;
struct ReadCutObservation {const char *stage="setup";unsigned sectors=0,event=0,events=0,completed=0;int phase=-1;};
static ReadCutObservation readCut;
static unsigned snapshotCalls=0;
static void readContext(const char *stage,unsigned sectors,unsigned event=0,unsigned events=0,int phase=-1,unsigned completed=0){readCut={stage,sectors,event,events,completed,phase};}
// Exact delegation only: retain numeric/key-name diagnostics, never value bytes.
// NvsBootSettings still reads the original settings/NVS engine directly.
struct DiagnosticSettings:mt::SettingsBackend {
  mt::ZephyrSettings &settings;
  explicit DiagnosticSettings(mt::ZephyrSettings &s):settings(s){}
  int read(const char *name,uint8_t *out,size_t cap)override{return settings.read(name,out,cap);}
  int write(const char *name,const uint8_t *data,size_t n)override{
    const int result=settings.write(name,data,n);
    lastBackendWrite.key=name;lastBackendWrite.bytes=n;lastBackendWrite.result=result;++lastBackendWrite.attempts;
    return result;
  }
};
static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
static Bytes pattern(size_t n,unsigned generation){Bytes b(n);for(size_t i=0;i<n;++i)b[i]=uint8_t(i*29+generation*17+(generation>>8));return b;}
struct Context:PkiStorageContext {
  unsigned depth=0;bool permit=true;
  bool tryEnter()override{++depth;return true;}
  void leave()override{CHECK(depth);--depth;}
  bool bootQuiescent()const override{return depth&&permit;}
  bool quiescent()const override{return depth&&permit;}
};
struct Entropy:EntropySource {
  unsigned calls=0;
  bool fill(uint8_t *p,size_t n)override{CHECK(n==4);++calls;std::memset(p,0x3a,n);return true;}
};
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+23*i);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(binding.publicKey,scalar);binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  ~Key(){eraseIdentityBytes(scalar,32);}
  IdentityCandidate candidate()const{return IdentityCandidate(binding,scalar);}
};
static Bytes value(mt::ZephyrSettings &settings,const char *name){
  Bytes b(NewRecord);const int n=settings.read(name,b.data(),b.size());
  if(n<=0||n>int(b.size()))std::fprintf(stderr,"M69 read counterexample: key=%s read_rc=%d capacity=%zu stage=%s sectors=%u event=%u total_events=%u phase=%d completed_cuts=%u snapshot_calls=%u raw_mutations=%u last_backend_write_rc=%d last_backend_write_bytes=%zu last_backend_write_key=%s\n",name,n,b.size(),readCut.stage,readCut.sectors,readCut.event,readCut.events,readCut.phase,readCut.completed,snapshotCalls,model_writes(),lastBackendWrite.result,lastBackendWrite.bytes,lastBackendWrite.key.c_str());
  CHECK(n>0&&n<=int(b.size()));b.resize(size_t(n));return b;
}
static void verify(mt::ZephyrSettings &settings,const Rows &expected){for(const auto &r:expected)CHECK(value(settings,r.first.c_str())==r.second);}
static bool erasedBits(const std::array<uint8_t,32768> &before,const std::array<uint8_t,32768> &after,unsigned sectors){for(size_t i=0;i<sectors*4096;++i)if(uint8_t(~before[i])&after[i])return true;return false;}
static void operation(bool ok,const char *name,unsigned sectors,unsigned index){if(!ok)std::fprintf(stderr,"M69 capacity counterexample: sectors=%u operation=%s index=%u raw_mutations=%u last_backend_write_rc=%d last_backend_write_bytes=%zu last_backend_write_key=%s backend_write_attempts=%u\n",sectors,name,index,model_writes(),lastBackendWrite.result,lastBackendWrite.bytes,lastBackendWrite.key.c_str(),lastBackendWrite.attempts);CHECK(ok);}
struct Fixture {
  mt::ZephyrSettings settings;DiagnosticSettings diagnostic{settings};Context context;Entropy entropy;Key local{19};
  std::array<Key,4> peers{{Key(71),Key(113),Key(157),Key(211)}};Rows protectedRows;
  // Reference legacy encoder: two TX IDs in newest slot, one RX per peer.
  Bytes legacy(unsigned tx){
    CHECK(tx==1||tx==2);Bytes b(1752,0);std::memcpy(b.data(),"ZTPL",4);b[4]=1;b[5]=128;b[6]=4;b[7]=64;
    put32(b.data()+8,1+tx+4+4);put32(b.data()+12,local.binding.node);std::memcpy(b.data()+16,local.binding.publicKey,32);b[48]=uint8_t(tx);b[49]=4;
    for(unsigned i=0;i<tx;++i)put32(b.data()+52+i*4,0x10000001u+i);
    for(unsigned p=0;p<4;++p){auto *r=b.data()+564+p*296;put32(r,peers[p].binding.node);std::memcpy(r+4,peers[p].binding.publicKey,32);r[36]=1;put32(r+40,0x20000001u+p*1024);}
    put32(b.data()+1748,crc(b.data(),1748));return b;
  }
  void populate(){
    for(unsigned i=0;i<5;++i){auto b=pattern(publicSizes[i],i+31);CHECK(!settings.write(publicNames[i],b.data(),b.size()));protectedRows[publicNames[i]]=b;}
    IdentityJournal identity;MonocypherIdentityDeriver deriver;CHECK(!identity.load(settings,deriver));CHECK(!identity.store(settings,deriver,local.candidate(),true));
    auto id=value(settings,"mtidentity/state0");CHECK(id.size()==84);CHECK(!settings.write("mtidentity/state1",id.data(),id.size()));protectedRows["mtidentity/state0"]=id;protectedRows["mtidentity/state1"]=id;
    for(unsigned slot=0;slot<2;++slot){auto b=legacy(slot+1);CHECK(!settings.write(ledgerNames[slot],b.data(),b.size()));}
  }
  KeyRef activate(PkiStorage &storage){
    const auto writes=model_writes();std::array<uint8_t,32768> before{},after{};model_copy_flash(before.data());CHECK(!storage.prepare());model_copy_flash(after.data());CHECK(writes==model_writes()&&before==after);
    IdentityBinding b;CHECK(storage.preparedBinding(b)&&b.node==local.binding.node&&!std::memcmp(b.publicKey,local.binding.publicKey,32));CHECK(!storage.activate(local.binding.node));KeyRef ref;CHECK(storage.runtimeIdentity()&&storage.runtimeIdentity()->binding(b,ref));return ref;
  }
  void oldHistory(PkiStorage &storage,KeyRef ref){
    uint32_t extra=99;const auto writes=model_writes(),calls=entropy.calls;
    CHECK(!storage.reserve(ref,local.binding.publicKey,local.binding.node,0x10000001u,extra));CHECK(!storage.reserve(ref,local.binding.publicKey,local.binding.node,0x10000002u,extra));CHECK(extra==99&&writes==model_writes()&&calls==entropy.calls);
    for(unsigned p=0;p<4;++p)CHECK(storage.seen(peers[p].binding.node,peers[p].binding.publicKey,0x20000001u+p*1024)==ReplayCheck::Seen);
  }
  void fullHistory(){
    NvsBootSettings boot;PkiStorage storage(boot,diagnostic,context,entropy);auto ref=activate(storage);oldHistory(storage,ref);uint32_t extra=99;const auto writes=model_writes();
    CHECK(!storage.reserve(ref,local.binding.publicKey,local.binding.node,0x71000001u,extra));CHECK(!storage.reserve(ref,local.binding.publicKey,local.binding.node,0x10000001u+NewTx-1,extra));
    for(unsigned p=0;p<4;++p){CHECK(storage.seen(peers[p].binding.node,peers[p].binding.publicKey,0x20000001u+NewRx-1+p*1024)==ReplayCheck::Seen);CHECK(storage.seen(peers[p].binding.node,peers[p].binding.publicKey,0x72000001u)==ReplayCheck::Full);}
    CHECK(writes==model_writes()&&extra==99);CHECK(storage.close());
  }
};
static void run(unsigned sectors){
  readContext("populate",sectors);model_reset(sectors);Fixture f;f.populate();verify(f.settings,f.protectedRows);model_snapshot();++snapshotCalls;unsigned events=0;
  {
    NvsBootSettings boot;PkiStorage s(boot,f.diagnostic,f.context,f.entropy);auto ref=f.activate(s);f.oldHistory(s,ref);uint32_t extra=99;model_fail(0,0);
    operation(s.reserve(ref,f.local.binding.publicKey,f.local.binding.node,0x33330001u,extra),"first-v1-to-v2-TX",sectors,0);CHECK(extra==0x3a3a3a3a);events=model_events();CHECK(events>0);auto migrated=value(f.settings,"mtpki/ledger0");CHECK(migrated.size()==NewRecord&&migrated[4]==2&&get32(migrated.data()+8)==12);CHECK(s.close());verify(f.settings,f.protectedRows);
  }
  unsigned cutCases=0;
  for(unsigned event=1;event<=events;++event)for(int phase=0;phase<3;++phase){
    readContext("first-v2-cut",sectors,event,events,phase,cutCases);
    model_restore();bool exposed=false;
    {
      NvsBootSettings boot;PkiStorage s(boot,f.diagnostic,f.context,f.entropy);auto ref=f.activate(s);const auto calls=f.entropy.calls;uint32_t extra=99;model_fail(event,phase);exposed=s.reserve(ref,f.local.binding.publicKey,f.local.binding.node,0x33330001u,extra);
      if(!exposed){CHECK(extra==99&&calls==f.entropy.calls&&s.state()==PkiStorage::State::Frozen&&!s.runtimeIdentity());const auto writes=model_writes();model_no_fault();CHECK(!s.reserve(ref,f.local.binding.publicKey,f.local.binding.node,0x33330002u,extra)&&writes==model_writes());}
      else CHECK(extra==0x3a3a3a3a);
    }
    model_no_fault();model_remount();verify(f.settings,f.protectedRows);
    NvsBootSettings boot;PkiStorage again(boot,f.diagnostic,f.context,f.entropy);const int rc=again.prepare();CHECK(rc==0||rc==-EILSEQ||rc==-ENODATA||rc==-EIO);
    if(!rc){CHECK(!again.activate(f.local.binding.node));IdentityBinding b;KeyRef ref;CHECK(again.runtimeIdentity()->binding(b,ref));f.oldHistory(again,ref);if(exposed){uint32_t extra=99;CHECK(!again.reserve(ref,b.publicKey,b.node,0x33330001u,extra)&&extra==99);}}
    ++cutCases;
  }
  ++groups;std::printf("PASS %u-sector migration: %u raw before/partial/after cuts; old histories/public journals preserved; exposed nonce never reusable\n",sectors,cutCases);
  // Start again from the exact synthetic legacy image, then consume every slot.
  readContext("full-capacity",sectors);
  model_restore();
  {
    NvsBootSettings boot;PkiStorage s(boot,f.diagnostic,f.context,f.entropy);auto ref=f.activate(s);f.oldHistory(s,ref);uint32_t extra=99;
    for(unsigned i=2;i<NewTx;++i)operation(s.reserve(ref,f.local.binding.publicKey,f.local.binding.node,0x10000001u+i,extra),"TX-fill",sectors,i+1);
    CHECK(!s.reserve(ref,f.local.binding.publicKey,f.local.binding.node,0x71000001u,extra));
    for(unsigned p=0;p<4;++p)for(unsigned i=1;i<NewRx;++i)operation(s.commitAuthenticated(f.peers[p].binding.node,f.peers[p].binding.publicKey,0x20000001u+p*1024+i),"RX-fill",sectors,p*NewRx+i+1);
    CHECK(s.close());
  }
  verify(f.settings,f.protectedRows);f.fullHistory();for(const char *n:ledgerNames){auto b=value(f.settings,n);CHECK(b.size()==NewRecord);f.protectedRows[n]=b;}
  ++groups;std::printf("PASS %u-sector full9-value capacity: both%zuB ledgers,%uTX/%uRX across4 pins, exact reload denial\n",sectors,NewRecord,NewTx,NewRx);
  bool foundGC=false;unsigned gcEvents=0,gcTarget=0,observedEraseOperations=0;Rows gcBefore;Bytes gcValue;
  const unsigned firstWrites=model_writes();
  for(unsigned i=0;i<1200;++i){
    readContext("public-GC-workload",sectors,i+1,1200);
    const unsigned target=i%31==0?0:i%7==0?3+i%2:1+i%2;const auto beforeRows=f.protectedRows;
    std::array<uint8_t,32768> before{},after{};model_copy_flash(before.data());if(!foundGC){model_snapshot();++snapshotCalls;}model_fail(0,0);auto b=pattern(publicSizes[target],i+100);
    operation(!f.settings.write(publicNames[target],b.data(),b.size()),"public-GC-workload",sectors,i);model_copy_flash(after.data());f.protectedRows[publicNames[target]]=b;verify(f.settings,f.protectedRows);
    if(erasedBits(before,after,sectors)){++observedEraseOperations;if(!foundGC){foundGC=true;gcEvents=model_events();gcTarget=target;gcBefore=beforeRows;gcValue=b;}}
    if(i%19==0){model_remount();f.fullHistory();verify(f.settings,f.protectedRows);}
  }
  CHECK(foundGC&&gcEvents>0&&observedEraseOperations>1);const unsigned workloadMutations=model_writes()-firstWrites;
  ++groups;std::printf("PASS %u-sector1200 public updates: raw_mutations=%u operations_with_proven_erase=%u; complete ledgers/public/identity rows checked each update\n",sectors,workloadMutations,observedEraseOperations);
  std::printf("INFO %u-sector selected actual-GC snapshot: raw_events=%u target=%s planned_cuts=%u snapshot_calls=%u\n",sectors,gcEvents,publicNames[gcTarget],3*gcEvents,snapshotCalls);
  // Prove the saved pre-GC image itself survives restoration/remount, then
  // prove the same exact pending update/remount without an injected error.
  // Never replace the saved snapshot: each later cut starts from this image.
  readContext("selected-GC-snapshot-prefault",sectors,0,gcEvents);
  model_restore();verify(f.settings,gcBefore);f.fullHistory();
  model_fail(0,0);
  operation(!f.settings.write(publicNames[gcTarget],gcValue.data(),gcValue.size()),"unfaulted-selected-GC-update",sectors,0);
  model_no_fault();model_remount();Rows unfaulted=gcBefore;unfaulted[publicNames[gcTarget]]=gcValue;
  readContext("selected-GC-unfaulted-remount",sectors,0,gcEvents);
  verify(f.settings,unfaulted);f.fullHistory();
  ++groups;std::printf("PASS %u-sector selected-GC snapshot and same unfaulted update survive exact public/identity/ledger readback after remount\n",sectors);
  unsigned gcCuts=0,latchedCases=0,latchedPhases=0;
  for(unsigned event=1;event<=gcEvents;++event)for(int phase=0;phase<3;++phase){
    readContext("actual-GC-cut",sectors,event,gcEvents,phase,gcCuts);
    model_restore();model_fail(event,phase);const int result=f.settings.write(publicNames[gcTarget],gcValue.data(),gcValue.size());
    if(result&&!model_ready()){
      // Clear fault injection: denial must come from the actual engine latch.
      model_no_fault();const auto mutations=model_writes();
      CHECK(f.settings.write(publicNames[gcTarget],gcValue.data(),gcValue.size())<0);
      CHECK(!model_ready()&&model_writes()==mutations);++latchedCases;latchedPhases|=1u<<phase;
    }
    model_no_fault();model_remount();
    for(const auto &r:gcBefore){auto b=value(f.settings,r.first.c_str());if(r.first==publicNames[gcTarget])CHECK(b==r.second||b==gcValue);else CHECK(b==r.second);}
    f.fullHistory();++gcCuts;
  }
  CHECK(latchedCases>0&&latchedPhases==7);
  ++groups;std::printf("PASS %u-sector actual-GC interruption: %u before/partial/after cuts; %u engine-unready cases reject second write without NOR mutation before remount; both complete ledgers/identity/unrelated public records exact\n",sectors,gcCuts,latchedCases);
}
int main(){
  static_assert(PkiLedger::LegacyRecordSize==1752&&PkiLedger::RecordSize==NewRecord&&PkiLedger::SendLimit==NewTx&&PkiLedger::ReceiveLimit==NewRx&&PkiLedger::PeerLimit==4,"independent144/72/1944 M69 contract");
  run(4);run(8);std::printf("{\"PASS\":true,\"groups\":%u,\"assertions\":%u,\"public_updates_per_geometry\":1200,\"geometries\":[4,8],\"exact_erase_count_measured\":false,\"hardware\":false}\n",groups,checks);
}
