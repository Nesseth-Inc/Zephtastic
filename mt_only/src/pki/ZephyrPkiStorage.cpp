// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrPkiStorage.h"
#include "PkiContactSnapshot.h"
#include "PkiContactMigrationMarker.h"
#include "provisioning/FreshDomain.h"
#include <string.h>

#if !defined(CONFIG_SETTINGS_NVS) || !CONFIG_SETTINGS_NVS
#error "ZephyrPkiStorage requires the pinned SETTINGS_NVS backend"
#endif

namespace mt::pki {
namespace {
constexpr uint16_t Counter=0x8000,Values=0x4000;
const char *keys[]={"mtidentity/state0","mtidentity/state1","mtpki/ledger0","mtpki/ledger1","mtpki/nonces0","mtpki/nonces1","mtpki/contacts0","mtpki/contacts1",PkiFreshProvisioning::RootKey,PkiFreshIntent::Key};
int keyIndex(const char *name){if(name)for(unsigned i=0;i<10;++i)if(!strcmp(name,keys[i]))return int(i);return -1;}
size_t recordSize(unsigned i){return i<2?IdentityJournal::RecordSize:i<4?PkiLedger::RecordSize:i<6?PkiNonceCounter::RecordBytes:i<8?PkiContactSnapshot::RecordBytes:i==8?PkiFreshRoot::RecordBytes:PkiFreshIntent::RecordBytes;}
bool supportedSize(unsigned i,int n){
  if(n==int(recordSize(i)))return true;
  if(i<2||i>=4)return false;
  return n==int(PkiLedger::LegacyRecordSize)||
         n==int(PkiContactMigrationMarker::RecordBytes)||
         n==int(PkiContactSnapshot::RecordBytes);
}
uint32_t hash(const uint8_t *p,size_t n){uint32_t h=2166136261u;for(size_t i=0;i<n;++i){h^=p[i];h*=16777619u;}return h;}
template<size_t N>struct Scratch {uint8_t bytes[N]{};~Scratch(){eraseIdentityBytes(bytes,N);}};
}
int NvsBootSettings::scan(ScanProgress progress, void *context){
  diagnostic_.stage=3;
  Scratch<2> count;const int n=int(nvs_read(fs_,Counter,count.bytes,2));
  if(n<0&&n!=-ENOENT)return n;
  if(n!=-ENOENT&&n!=2)return -EILSEQ;
  const uint16_t last=n==-ENOENT?Counter:uint16_t(count.bytes[0]|uint16_t(count.bytes[1])<<8);
  diagnostic_.lastNameId=last;diagnostic_.stage=4;
  if(last<Counter||last>Counter+NameLimit)return -EILSEQ;
  Scratch<NameBytes+1> name;Scratch<1> sample;
  uint32_t hashes[NameLimit]{};uint8_t lengths[NameLimit]{};unsigned names=0;
  for(unsigned slot=1;slot<=NameLimit;++slot){
    // The preceding 16 name/value pairs have both been checked, including holes.
    if(progress && slot>1 && (slot-1)%16==0)progress(context);
    diagnostic_.stage=5;diagnostic_.slot=uint16_t(slot);diagnostic_.kind=0;
    const uint16_t id=uint16_t(Counter+slot);
    const int length=int(nvs_read(fs_,id,name.bytes,NameBytes+1));
    const int value=int(nvs_read(fs_,uint16_t(id+Values),sample.bytes,1));
    diagnostic_.nameLength=length;diagnostic_.valueLength=value;
    if(length<0&&length!=-ENOENT)return length;
    if(value<0&&value!=-ENOENT)return value;
    // A settings writer may durably advance the name counter before it
    // stores a new name/value pair. Treat an empty high-water ID like any
    // other hole, without repairing NVS. A name-only or value-only row still
    // fails the validation below.
    if(length==-ENOENT&&value==-ENOENT)continue;
    diagnostic_.stage=6;
    if(length<=0||value<=0||id>last||length>int(NameBytes)||memchr(name.bytes,0,size_t(length)))return -EILSEQ;
    const auto h=hash(name.bytes,size_t(length));
    // Duplicate-name/hash collision is rejected, never resolved by preference.
    // Fixed metadata memory and fixed scan count avoid unbounded name searches.
    diagnostic_.stage=7;
    for(unsigned i=0;i<names;++i)if(hashes[i]==h&&lengths[i]==length)return -EILSEQ;
    hashes[names]=h;lengths[names++]=uint8_t(length);name.bytes[length]=0;
    diagnostic_.names=uint16_t(names);diagnostic_.stage=8;
    diagnostic_.valueBytes=uint32_t(value)>UINT32_MAX-diagnostic_.valueBytes?UINT32_MAX:diagnostic_.valueBytes+uint32_t(value);
    diagnostic_.kind=(!strcmp(reinterpret_cast<const char *>(name.bytes),"mtextra/state0")||
      !strcmp(reinterpret_cast<const char *>(name.bytes),"mtextra/state1"))?2:
      (!strcmp(reinterpret_cast<const char *>(name.bytes),"mtcanned/state0")||
       !strcmp(reinterpret_cast<const char *>(name.bytes),"mtcanned/state1"))?3:0;
    const int index=keyIndex(reinterpret_cast<const char *>(name.bytes));
    if(index>=0){if(ids_[index]||!supportedSize(unsigned(index),value))return -EILSEQ;ids_[index]=id;}
  }
  diagnostic_.stage=9;
  if(progress)progress(context);
  return 0;
}
int NvsBootSettings::begin(){return beginWithProgress(nullptr,nullptr);}
int NvsBootSettings::beginWithProgress(ScanProgress progress, void *context){
  if(held_)return -EBUSY;
  diagnostic_={};diagnostic_.stage=1;
  void *storage=nullptr;const int result=settings_storage_get(&storage);
  if(result){diagnostic_.result=result;return result;}
  if(!storage){diagnostic_.result=-ENODEV;return -ENODEV;}
  fs_=static_cast<nvs_fs *>(storage);
  diagnostic_.stage=2;
  if(!fs_->ready||fs_->sector_size!=4096||(fs_->sector_count!=4&&fs_->sector_count!=8)){fs_=nullptr;diagnostic_.result=-EACCES;return -EACCES;}
  if(k_mutex_lock(&fs_->nvs_lock,K_NO_WAIT)){fs_=nullptr;diagnostic_.result=-EBUSY;return -EBUSY;}
  held_=true;
  if(!fs_->ready){end();diagnostic_.result=-EACCES;return -EACCES;}
  const int rc=scan(progress,context);diagnostic_.result=rc;if(rc)end();return rc;
}
void NvsBootSettings::end(){
  eraseIdentityBytes(ids_,sizeof(ids_));
  if(held_)k_mutex_unlock(&fs_->nvs_lock);
  held_=false;fs_=nullptr;
}
int NvsBootSettings::read(const char *name,uint8_t *out,size_t capacity){
  const int index=keyIndex(name);
  if(!held_||index<0||!out||capacity!=recordSize(unsigned(index)))return -EACCES;
  if(!ids_[index]){eraseIdentityBytes(out,capacity);return -ENOENT;}
  const int n=int(nvs_read(fs_,uint16_t(ids_[index]+Values),out,capacity));
  if(!supportedSize(unsigned(index),n)){eraseIdentityBytes(out,capacity);return n<0?n:-EILSEQ;}
  return n;
}
bool ZephyrPkiContext::tryEnter(){if(k_mutex_lock(&mutex_,K_NO_WAIT))return false;++depth_;return true;}
void ZephyrPkiContext::leave(){if(depth_){--depth_;k_mutex_unlock(&mutex_);}}
bool ZephyrPkiContext::bind(Service &service){
  if(!tryEnter())return false;
  const bool ok=!service_||service_==&service;if(ok)service_=&service;leave();return ok;
}
bool ZephyrPkiContext::bootQuiescent()const{
  return depth_&&radio_.ready()&&!radio_.busy()&&radio_.health()==0&&(!service_||service_->storageQuiescent());
}
bool ZephyrPkiContext::quiescent()const{return depth_&&service_&&!radio_.busy()&&service_->privateStoragePaused();}
bool ZephyrPkiContext::identityRecoveryQuiescent()const{
  return depth_&&service_&&radio_.ready()&&!radio_.busy()&&radio_.health()==0&&service_->identityRecoveryQuiescent();
}
}
