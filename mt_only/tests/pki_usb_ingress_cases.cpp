// SPDX-License-Identifier: GPL-3.0-only
// Host-only public deterministic fixtures; no device, live RNG or credentials.
#include "pki/PkiProvisioning.h"
#include "pki/PkiUsbIngress.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <vector>
using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
using Rows=std::map<std::string,Bytes>;
struct Memory:mt::SettingsBackend {
  Rows rows;unsigned writes=0;int fault=0;unsigned at=0;size_t cut=0;std::vector<std::string> order;
  std::function<void()> onWrite;
  int read(const char *key,uint8_t *out,size_t capacity)override{
    auto i=rows.find(key);if(i==rows.end())return -ENOENT;
    if(!i->second.empty())memcpy(out,i->second.data(),std::min(capacity,i->second.size()));
    return int(i->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t n)override{
    ++writes;order.emplace_back(key);if(onWrite)onWrite();
    if(writes==at&&fault==1)return -EIO;
    rows[key]={data,data+((writes==at&&fault==3)?std::min(cut,n):n)};
    return writes==at&&fault==2?-EIO:0;
  }
};
struct Boot:ProvisioningSettings {
  Memory &memory;Rows snapshot;bool held=false;unsigned reads=0,begins=0;int error=0;
  std::function<void()> onBegin,onRead;
  explicit Boot(Memory &m):memory(m){}
  int begin()override{assert(!held);++begins;if(onBegin)onBegin();if(error)return error;held=true;snapshot=memory.rows;return 0;}
  void end()override{assert(held);held=false;snapshot.clear();}
  int read(const char *key,uint8_t *out,size_t capacity)override{
    assert(held);++reads;if(onRead)onRead();
    if(memory.at&&memory.at==memory.writes&&memory.fault==4)return -EIO;
    auto i=snapshot.find(key);if(i==snapshot.end())return -ENOENT;
    if(!i->second.empty())memcpy(out,i->second.data(),std::min(capacity,i->second.size()));
    if(memory.at&&memory.at==memory.writes&&memory.fault==5&&capacity)out[0]^=1;
    return int(i->second.size());
  }
  int persist(const char *key,const uint8_t *data,size_t size)override{return memory.write(key,data,size);}
};
struct Context:ProvisioningContext,PkiStorageContext {
  bool permit=true,lease=true,candidate=true,peer=true;uint64_t now=100;unsigned depth=0;
  std::function<void()> onCandidate,onPeer;
  bool tryEnter()override{if(!lease)return false;++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool quiescent()const override{return depth&&permit;}
  bool bootQuiescent()const override{return quiescent();}
  uint64_t millis()const override{return now;}
  bool candidateAllowed(const IdentityBinding &)const override{if(onCandidate)onCandidate();return candidate;}
  bool peerAllowed(const IdentityBinding &,const ProvisionPeer &)const override{if(onPeer)onPeer();return peer;}
};
struct Entropy:EntropySource {
  uint8_t raw[32]{};unsigned calls=0;bool fail=false;std::function<void()> hook;
  explicit Entropy(unsigned seed=19){for(unsigned i=0;i<32;++i)raw[i]=uint8_t(seed+23*i);}
  bool fill(uint8_t *out,size_t n)override{assert(n==32);++calls;memcpy(out,raw,n);if(hook)hook();return !fail;}
};
struct Key {
  uint8_t scalar[32]{};IdentityBinding binding;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+23*i);
    scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(binding.publicKey,scalar);
    binding.node=bench::MtSignature::nodeId(binding.publicKey);binding.generation=1;}
  ~Key(){eraseIdentityBytes(scalar,32);}
  ProvisionPeer pin()const{ProvisionPeer p;p.node=binding.node;memcpy(p.publicKey,binding.publicKey,32);return p;}
  IdentityCandidate candidate()const{return IdentityCandidate(binding,scalar);}
};

// Synthetic policy boundary, not a claim about CDC/physical-owner admission.
struct Session:ProvisionUsbSession {
  uint32_t live=7,bound=0;bool connected=true;unsigned binds=0,clears=0;
  bool bind(uint32_t epoch)override{++binds;if(!connected||!epoch||epoch!=live||(bound&&bound!=epoch))return false;bound=epoch;return true;}
  bool current(uint32_t epoch)const override{return connected&&bound&&bound==live&&bound==epoch;}
  bool current()const override{return current(bound);}
  void clear()override{bound=0;++clears;}
};
struct UsbContext:Context {
  Session &session;explicit UsbContext(Session &s):session(s){}
  bool quiescent()const override{return Context::quiescent()&&session.current();}
  bool bootQuiescent()const override{return quiescent();}
};
static std::string hex(const uint8_t *p,size_t n){constexpr char alphabet[]="0123456789abcdef";std::string out;for(size_t i=0;i<n;++i){out+=alphabet[p[i]>>4];out+=alphabet[p[i]&15];}return out;}
static std::string node(uint32_t value){char out[9]{};assert(snprintf(out,sizeof(out),"%08x",unsigned(value))==8);return out;}
struct UsbFixture {
  Memory memory;Boot boot{memory};Session session;UsbContext context{session};Entropy entropy;Key local{19},peer{71};
  PkiProvisioning core{boot,context,entropy};PkiUsbIngress ingress{core,session};
  UsbFixture(){memory.rows["mtbase/state0"]=Bytes(112,0xab);memory.rows["mtbase/nodes0"]=Bytes(1492,0x56);}
  std::string execute(const std::string &command,uint32_t epoch=7){
    char out[256]{};const int n=ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),epoch,out,sizeof(out));
    assert(n>0&&size_t(n)==strlen(out)&&size_t(n)<sizeof(out)&&out[n]==0);
    for(int i=0;i<n;++i)assert(out[i]>=32&&out[i]<=126);
    const std::string result(out,size_t(n));assert(result.find(hex(local.scalar,32))==std::string::npos);
    assert(result.find("private")==std::string::npos&&result.find("scalar")==std::string::npos);
    return result;
  }
  int code(const std::string &result){int rc=123;assert(sscanf(result.c_str(),"pki_v1 rc=%d",&rc)==1);return rc;}
  std::string confirmation(const IdentityBinding &expected,const ProvisionPeer &pin){return "zt_pki_commit_v1 "+node(expected.node)+" "+hex(expected.publicKey,32)+" "+node(pin.node)+" "+hex(pin.publicKey,32);}
  void stage(){const auto result=execute("zt_pki_stage_v1");assert(code(result)==0&&core.state()==ProvisionState::Staged&&entropy.calls==1&&!memory.writes);
    assert(result.find("local=1 node="+node(local.binding.node)+" pub="+hex(local.binding.publicKey,32))!=std::string::npos);}
};
static void pass(const char *name){std::cout<<"PASS "<<name<<'\n';}

int main(){
  {UsbFixture f;const auto rows=f.memory.rows;f.ingress.tick();f.ingress.disconnect();f.ingress.tick();
    assert(f.core.state()==ProvisionState::New&&!f.entropy.calls&&!f.memory.writes&&f.memory.rows==rows);
    f.stage();const auto again=f.execute("zt_pki_stage_v1");assert(f.code(again)==0&&f.entropy.calls==1&&!f.memory.writes);
    pass("firstboot-New-tick-and-disconnect-remain-stageable-with-one-entropy-draw");}
  {UsbFixture f;f.session.connected=false;char out[256]="UNCHANGED";const std::string command="zt_pki_stage_v1";
    assert(f.ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),7,out,sizeof(out))==-EACCES);
    assert(std::string(out)=="UNCHANGED"&&!f.entropy.calls&&!f.memory.writes&&!f.boot.reads);
    f.session.connected=true;f.stage();pass("no-session-admission-has-no-read-write-entropy-or-response");}
  {UsbFixture f;f.context.permit=false;const auto before=f.memory.rows;const auto denied=f.execute("zt_pki_stage_v1");
    assert(f.code(denied)<0&&!f.entropy.calls&&!f.memory.writes&&f.memory.rows==before);
    pass("actual-context-denial-before-generation-or-persistence");}
  for(unsigned mode=0;mode<3;++mode){UsbFixture f;f.stage();
    if(mode==0)f.ingress.disconnect();
    if(mode==1){f.session.connected=false;f.ingress.tick();}
    if(mode==2){f.context.now+=PkiProvisioning::StageLifetimeMs;f.ingress.tick();}
    assert(f.core.state()==(mode==2?ProvisionState::Expired:ProvisionState::Cancelled));
    assert(!f.memory.writes&&f.entropy.calls==1);f.session.connected=true;
    const auto retry=f.execute("zt_pki_stage_v1");assert(f.code(retry)<0&&f.entropy.calls==1&&!f.memory.writes);
  }
  pass("staged-disconnect-epoch-loss-and-expiry-are-terminal-with-no-regeneration");
  {UsbFixture f;f.stage();f.session.live=8;char out[256]="UNCHANGED";const std::string command=f.confirmation(f.local.binding,f.peer.pin());
    assert(f.ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),8,out,sizeof(out))==-EACCES);
    assert(f.core.state()==ProvisionState::Cancelled&&!f.memory.writes&&std::string(out)=="UNCHANGED");
    pass("different-live-USB-epoch-cannot-commit-staged-candidate");}
  {UsbFixture f;const auto publicBefore=f.memory.rows;f.stage();const auto result=f.execute(f.confirmation(f.local.binding,f.peer.pin()));
    assert(f.code(result)==0&&f.core.state()==ProvisionState::Committed&&f.memory.writes==3);
    assert((f.memory.order==std::vector<std::string>{"mtpki/ledger0","mtpki/ledger1","mtidentity/state0"}));
    for(const auto &r:publicBefore)assert(f.memory.rows.at(r.first)==r.second);
    ProvisionStatus state;assert(!f.core.inspect(state)&&state.stored==ProvisionStored::Complete&&state.hasBinding&&state.hasPeer);
    assert(state.local.node==f.local.binding.node&&state.local.generation==1&&state.peer.node==f.peer.binding.node&&!memcmp(state.peer.publicKey,f.peer.binding.publicKey,32));
    const auto writes=f.memory.writes;const auto repeat=f.execute(f.confirmation(f.local.binding,f.peer.pin()));assert(f.code(repeat)<0&&f.memory.writes==writes);
    pass("exact-public-confirmed-commit-writes-domain-peer-identity-only-and-never-repeats");}
  for(unsigned mode=0;mode<5;++mode){UsbFixture f;f.stage();auto local=f.local.binding;auto peer=f.peer.pin();
    if(mode==0){++local.node;}
    if(mode==1){local.publicKey[4]^=1;}
    if(mode==2){++peer.node;}
    if(mode==3){peer=f.local.pin();}
    if(mode==4){f.context.peer=false;}
    const auto before=f.memory.rows;const auto command=f.confirmation(local,peer);
    if(mode==3){char out[256]="UNCHANGED";assert(f.ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),7,out,sizeof(out))==-EINVAL&&std::string(out)=="UNCHANGED"&&f.core.state()==ProvisionState::Staged);}
    else {const auto result=f.execute(command);assert(f.code(result)<0);}
    assert(!f.memory.writes&&f.memory.rows==before&&f.entropy.calls==1);
  }
  pass("changed-local-confirmation-peer-CRC-self-and-collision-policy-never-write");
  for(const std::string command:{"zt_pki_private_v1","zt_pki_export_v1","zt_pki_stage_v1 extra","zt_pki_commit_v1 1 bad 2 bad","zt_pki_status_v1\n"}){
    UsbFixture f;char out[256]="UNCHANGED";assert(f.ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),7,out,sizeof(out))==-EINVAL);
    assert(std::string(out)=="UNCHANGED"&&!f.memory.writes&&!f.entropy.calls&&!f.boot.reads);
  }
  {UsbFixture f;char out[223]="UNCHANGED";const std::string command="zt_pki_stage_v1";
    assert(f.ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),7,out,sizeof(out))==-EINVAL&&!f.entropy.calls&&!f.memory.writes);}
  pass("invalid-grammar-private-getters-and-small-output-deny-before-any-core-side-effect");
  {UsbFixture f;f.stage();const auto result=f.execute("zt_pki_cancel_v1");assert(f.code(result)==0&&f.core.state()==ProvisionState::Cancelled&&!f.memory.writes);
    const auto status=f.execute("zt_pki_status_v1");assert(f.code(status)==0&&f.entropy.calls==1&&!f.memory.writes);
    pass("explicit-cancel-and-public-status-never-export-or-persist-secret-material");}
  for(unsigned at=1;at<=3;++at)for(int fault:{1,2,4,5}){UsbFixture f;f.stage();f.memory.at=at;f.memory.fault=fault;
    const auto result=f.execute(f.confirmation(f.local.binding,f.peer.pin()));assert(f.code(result)<0&&f.core.state()==ProvisionState::Frozen&&f.memory.writes==at);
    const auto writes=f.memory.writes;assert(f.code(f.execute("zt_pki_stage_v1"))<0&&f.memory.writes==writes&&f.entropy.calls==1);
    f.memory.fault=0;PkiProvisioning reboot(f.boot,f.context,f.entropy);ProvisionStatus status;const int rc=reboot.inspect(status);
    if(at==3&&(fault==2||fault==4||fault==5))assert(!rc&&status.stored==ProvisionStored::Complete);
    else assert(rc<0||status.stored!=ProvisionStored::Complete);
    if(status.stored!=ProvisionStored::Empty){IdentityBinding unchanged;unchanged.node=123;assert(reboot.stage(unchanged)<0&&unchanged.node==123&&f.memory.writes==writes);}
    else assert(f.entropy.calls==1&&f.memory.writes==writes); // An empty fresh boot is observed, never automatically restaged.
  }
  pass("each-durable-write-or-readback-uncertainty-stays-negative-frozen-no-retry-despite-later-Complete");
  {UsbFixture f;f.memory.rows["mtpki/ledger0"]=Bytes(1,0x7f);const auto before=f.memory.rows;
    const auto result=f.execute("zt_pki_stage_v1");assert(f.code(result)<0&&!f.entropy.calls&&!f.memory.writes&&f.memory.rows==before);
    pass("preexisting-partial-or-corrupt-store-cannot-be-repaired-or-regenerated-by-ingress");}
  for(bool afterWrite:{false,true}){UsbFixture f;f.stage();
    if(afterWrite)f.memory.onWrite=[&]{f.session.live=8;};
    else f.context.onPeer=[&]{f.session.live=8;};
    char out[256]="UNCHANGED";const auto command=f.confirmation(f.local.binding,f.peer.pin());
    assert(f.ingress.execute(reinterpret_cast<const uint8_t *>(command.data()),command.size(),7,out,sizeof(out))==-EACCES);
    assert(std::string(out)=="UNCHANGED"&&f.memory.writes==(afterWrite?1u:0u));
    assert(f.core.state()==(afterWrite?ProvisionState::Frozen:ProvisionState::Rejected));
  }
  pass("epoch-loss-before-or-after-first-persist-refuses-response-and-stops-next-durable-step");
  return 0;
}
