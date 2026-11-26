// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "DeviceResetFloor.h"
#include "FactoryConfigReset.h"
#include "pki/provisioning/FreshDomain.h"

namespace mt::stock::admin::device_reset {
constexpr char RequestKey[]="mtreset/next",ReadyKey[]="mtreset/built";
struct Request {
  // Raw candidate exists only in this erased-on-release buffer and the
  // existing provisioning authority. No logs, external interfaces or exports.
  static constexpr unsigned Size=32+pki::PkiFreshIntent::RecordBytes+4;
  pki::FreshSecret<Size> wire;
  pki::IdentityBinding next{};
  const uint8_t *oldKey()const{return wire.bytes;}
  const uint8_t *intent()const{return wire.bytes+32;}
  bool validate(){
    pki::MonocypherIdentityDeriver derive;pki::FreshSecret<32> scalar;
    return pki::PkiFreshRoot::word(wire.bytes+Size-4)==pki::PkiFreshRoot::checksum(wire.bytes,Size-4)&&
      pki::PkiFreshIntent::decode(intent(),pki::PkiFreshIntent::RecordBytes,next,scalar.bytes,derive)&&
      memcmp(oldKey(),next.publicKey,32)!=0;
  }
  int generate(const uint8_t previous[32],pki::EntropySource &source){
    pki::FreshSecret<32> scalar;pki::MonocypherIdentityDeriver derive;
    if(!source.fill(scalar.bytes,32))return -ENODATA;
    scalar.bytes[0]&=248;scalar.bytes[31]=(scalar.bytes[31]&127)|64;
    next={};next.generation=1;
    if(!derive.derivePublic(scalar.bytes,next.publicKey))return -EIO;
    next.node=bench::MtSignature::nodeId(next.publicKey);
    memcpy(wire.bytes,previous,32);
    if(!pki::PkiFreshIntent::encode(next,scalar.bytes,wire.bytes+32))return -EINVAL;
    pki::PkiFreshRoot::put(wire.bytes+Size-4,pki::PkiFreshRoot::checksum(wire.bytes,Size-4));
    return validate()?0:-EINVAL;
  }
  template<class Store> int load(Store &store){
    const int n=store.read(RequestKey,wire.bytes,Size);
    return n<0?n:n==int(Size)&&validate()?0:-EILSEQ;
  }
};
enum class RootUse { VerifyReplacement, RetireCurrent };

template<class Store>
int freshBinding(Store &store,const pki::IdentityBinding &identity,RootUse use=RootUse::VerifyReplacement){
  uint8_t selector[pki::PkiFreshRoot::RecordBytes]{};
  pki::PkiFreshRoot root;
  const int n=store.read(pki::PkiFreshProvisioning::RootKey,selector,sizeof(selector));
  if(n<0)return n;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  // Only the outgoing identity can be imported. A floor-selected replacement
  // must still match the native fresh root built by the reset transaction.
  if(use==RootUse::RetireCurrent&&pki::ImportedDomain::matchesRoot(selector,size_t(n),identity,root)){
    pki::FreshSecret<pki::PkiFreshIntent::RecordBytes> pending;
    const int m=store.read(pki::PkiFreshIntent::Key,pending.bytes,sizeof(pending.bytes));
    if(m<0)return m;
    uint8_t salt[pki::ImportedDomain::SaltSize]{};
    if(m!=int(sizeof(pending.bytes))||!pki::ImportedDomain::readIntent(pending.bytes,identity,salt))return -EXDEV;
    return memcmp(salt,selector+4,sizeof(salt))?-EXDEV:0;
  }
#else
  (void)use;
#endif
  return pki::PkiFreshRoot::decode(selector,size_t(n),root)&&root.matches(identity)?0:-EXDEV;
}
template<class Flash,class Store> int begin(Flash &flash,Store &store,const pki::IdentityBinding &current,
                                            pki::EntropySource &entropy,bool &attempted){
  attempted=false;Floor::Head h;int rc=Floor::inspect(flash,h);if(rc)return rc;
  if((rc=freshBinding(store,current,RootUse::RetireCurrent)))return rc;
  if(h.state!=Floor::State::Unused&&h.state!=Floor::State::Active)return -EBUSY;
  if(h.count>=Floor::Slots)return -ENOSPC;
  if(h.state==Floor::State::Unused){if((rc=flash.firstEnrollment()))return rc;}
  else if(memcmp(h.newKey,current.publicKey,32))return -EACCES;
  Request request;if((rc=request.generate(current.publicKey,entropy)))return rc;
  if(request.next.node==current.node)return -EEXIST;
  // Check all retired keys before retiring the current one.
  for(unsigned i=0;i<h.count;++i){uint8_t retired[32];rc=flash.read(i*Floor::SlotBytes+8,retired,32);
    if(rc)return rc;if(!memcmp(request.next.publicKey,retired,32))return -EEXIST;}
  attempted=true;if((rc=store.write(RequestKey,request.wire.bytes,Request::Size)))return rc;
  Request check;if((rc=check.load(store)))return rc;
  if(memcmp(check.wire.bytes,request.wire.bytes,Request::Size))return -EIO;
  if((rc=Floor::retire(flash,h,current.publicKey)))return rc;
  if((rc=Floor::inspect(flash,h)))return rc;
  return Floor::select(flash,h,request.next.publicKey);
}
template<class Flash,class Store> int inspect(Flash &flash,Store &store,Floor::Head &head){
  unsigned bad=0;int rc=Floor::inspect(flash,head,&bad);
  if(rc&&rc!=-EILSEQ)return rc;
  if(!rc&&head.state!=Floor::State::Retiring)return 0;
  Request request;const int loaded=request.load(store);if(loaded)return loaded;
  // Erasure starts only after both floor records are verified. The previous
  // root must therefore still authorize a torn retirement or selection.
  pki::IdentityBinding old{};
  old.generation=1;old.node=bench::MtSignature::nodeId(request.oldKey());
  memcpy(old.publicKey,request.oldKey(),sizeof(old.publicKey));
  const int binding=freshBinding(store,old,RootUse::RetireCurrent);if(binding)return binding;
  if(rc&&bad==0){const int blank=flash.firstEnrollment(true);if(blank)return blank;}
  if(rc)rc=Floor::recoverTail(flash,head,bad,request.oldKey(),request.next.publicKey);
  else if(memcmp(head.oldKey,request.oldKey(),32))return -EACCES;
  else rc=Floor::select(flash,head,request.next.publicKey);
  return rc?rc:Floor::inspect(flash,head);
}
template<class Store> int ready(Store &s,const Floor::Head &h){
  uint8_t key[32];const int n=s.read(ReadyKey,key,sizeof(key));
  return n<0?n:n==32&&!memcmp(key,h.newKey,32)?0:-ESTALE;
}
template<class Store,class Bonds,class Files,class Install>
int rebuild(Store &s,const Floor::Head &h,Bonds bonds,Files files,Install install){
  if(h.state!=Floor::State::Selected)return -EACCES;
  Request req;int rc=req.load(s);if(rc)return rc;
  if(memcmp(req.oldKey(),h.oldKey,32)||memcmp(req.next.publicKey,h.newKey,32))return -EXDEV;
  // A fresh-domain reset never deletes a legacy ledger or append partition.
  for(unsigned i=0;i<2;++i){uint8_t byte;const int n=s.read(i?"mtpki/ledger1":"mtpki/ledger0",&byte,1);
    if(n!=-ENOENT)return n>=0||n==-E2BIG?-EXDEV:n;}
  if((rc=files())||(rc=bonds()))return rc;
  if((rc=factory::eraseStoredPreferences(s)))return rc;
  static const char *const keys[]={"mtba/m0","mtba/m1","mtba/ri","mtreset/config",ReadyKey,
    #if defined(CONFIG_SOC_ESP32S3)
    "mtinfo/boots",
#endif
    "mtidentity/state0","mtidentity/state1","mtpki/fresh0","mtpki/fresh-pending",
    "mtpki/contacts0","mtpki/contacts1","mtpki/nonces0","mtpki/nonces1"};
  for(const char *k:keys)if((rc=s.eraseVerified(k)))return rc;
  for(char bank='a';bank<='b';++bank){char key[]="mtba/a00";key[5]=bank;
    for(unsigned i=0;i<16;++i){key[6]=char('0'+i/10);key[7]=char('0'+i%10);if((rc=s.eraseVerified(key)))return rc;}}
  if((rc=s.write(pki::PkiFreshIntent::Key,req.intent(),pki::PkiFreshIntent::RecordBytes)))return rc;
  if((rc=install(req.next)))return rc;
  if((rc=s.write(ReadyKey,h.newKey,32)))return rc;
  return ready(s,h);
}
}
