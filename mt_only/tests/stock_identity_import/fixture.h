// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// In-memory private test inputs; emit only scenario outcomes.
#include "pki/provisioning/StockImportConsent.h"
#include "pki/PkiStorage.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace mt::pki;
using Blob=std::vector<uint8_t>;
struct Memory:mt::SettingsBackend {
  std::map<std::string,Blob> rows;
  unsigned writes=0,failAt=0;bool failAfter=false;
  int read(const char*k,uint8_t*out,size_t n)override{
    const auto it=rows.find(k);if(it==rows.end())return -ENOENT;
    if(it->second.size()>n)return -E2BIG;memcpy(out,it->second.data(),it->second.size());return it->second.size();
  }
  int eraseVerified(const char *k){rows.erase(k);return 0;}
  int write(const char*k,const uint8_t*p,size_t n)override{
    const bool fail=++writes==failAt;if(fail&&!failAfter)return -EIO;
    rows[k]={p,p+n};return fail?-EIO:0;
  }
};
struct Volume:FreshProvisioningSettings {
  Memory &mem;bool reading=false;explicit Volume(Memory &m):mem(m){}
  int begin()override{if(reading)return -EBUSY;reading=true;return 0;}
  void end()override{assert(reading);reading=false;}
  int read(const char*k,uint8_t*p,size_t n)override{
    // Match the exact buffer contract of the real NvsBootSettings scanner.
    if(!reading)return -EACCES;
    const size_t expected=!strncmp(k,"mtidentity/",11)?IdentityJournal::RecordSize:
      !strncmp(k,"mtpki/nonces",12)?PkiNonceCounter::RecordBytes:
      !strncmp(k,"mtpki/contacts",14)?PkiContactStore::RecordBytes:
      !strcmp(k,PkiFreshProvisioning::RootKey)?PkiFreshRoot::RecordBytes:
      !strcmp(k,PkiFreshIntent::Key)?PkiFreshIntent::RecordBytes:PkiLedger::RecordSize;
    return n==expected?mem.read(k,p,n):-EACCES;
  }
  int persist(const char*k,const uint8_t*p,size_t n)override{
    return reading||mem.rows.count(k)?-EEXIST:mem.write(k,p,n);
  }
};
struct Clock:ProvisioningContext,PkiStorageContext {
  unsigned depth=0;uint64_t now=100,until=0;uint32_t epoch=33,ble=51;bool allowed=true;
  bool tryEnter()override{++depth;return true;}
  void leave()override{assert(depth);--depth;}
  bool quiescent()const override{return depth&&allowed&&(!until||now<until);}
  bool bootQuiescent()const override{return quiescent();}
  uint64_t millis()const override{return now;}
  bool candidateAllowed(const IdentityBinding&)const override{return allowed;}
  bool peerAllowed(const IdentityBinding&,const ProvisionPeer&)const override{return false;}
  bool bind(uint32_t e)const{return current(e);}
  bool current(uint32_t e)const{return epoch==e&&(!until||now<until);}
  bool currentImportBle(uint32_t b)const{return b&&b==ble;}
  bool entropyAllowed=true;
  bool prepareHeldImportEntropy()const{return entropyAllowed;}
  void limitUntil(uint64_t d){until=d;}
  void clear(){until=0;}
};
struct Random:EntropySource {
  uint32_t x=1;bool fail=false;explicit Random(uint32_t seed):x(seed){}
  bool fill(uint8_t*out,size_t n)override{
    if(fail)return false;for(size_t i=0;i<n;i++){x=1664525u*x+1013904223u;out[i]=uint8_t(x>>24);}return true;
  }
};
struct Pair{
  std::array<uint8_t,32> scalar{},pub{};
  explicit Pair(unsigned seed){Random r(seed);assert(r.fill(scalar.data(),32));scalar[0]&=248;scalar[31]=(scalar[31]&63)|64;
    crypto_x25519_public_key(pub.data(),scalar.data());}
  ~Pair(){eraseIdentityBytes(scalar.data(),32);}
};
static std::string hex(const uint8_t*p,size_t n){
  std::string out;static const char h[]="0123456789abcdef";for(size_t i=0;i<n;i++){out+=h[p[i]>>4];out+=h[p[i]&15];}return out;
}
struct Provision {
  Clock clock;Random rng;Volume volume;PkiFreshProvisioning core;StockImportConsent<Clock> control;
  std::string challenge;IdentityBinding target{};
  Provision(Memory &m,unsigned seed):rng(seed),volume(m),core(volume,clock,rng),control(core,clock,rng){}
  int command(const std::string &verb,std::string &reply,bool eligible=true,bool held=false){
    std::string full="zt_pki_import_v1 "+verb;char buf[225]{};
    int n=control.command(reinterpret_cast<const uint8_t*>(full.data()),full.size(),clock.epoch,buf,sizeof(buf),true,eligible,held);
    if(n>0)reply.assign(buf,n);else reply.clear();eraseIdentityBytes(buf,sizeof(buf));return n;
  }
  void arm(bool held=false){std::string response;assert(command("arm",response,!held,held)>0);const auto p=response.find("challenge=");
    assert(p!=std::string::npos);challenge=response.substr(p+10);assert(challenge.size()==32);}
  int stage(const Pair &p,bool isUsb=true){return control.stage(p.scalar.data(),p.pub.data(),isUsb,isUsb?clock.epoch:clock.ble);}
  int confirm(const Pair &p){
    char node[9];snprintf(node,sizeof(node),"%08x",bench::MtSignature::nodeId(p.pub.data()));std::string reply;
    return command(std::string("confirm ")+node+" "+hex(p.pub.data(),32)+" "+challenge,reply);
  }
};
