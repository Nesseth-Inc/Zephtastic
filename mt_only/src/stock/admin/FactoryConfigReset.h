// SPDX-License-Identifier: GPL-3.0-only
// Zephyr storage adapter for Meshtastic NodeDB::factoryReset(false).
#pragma once
#include <errno.h>
#include <stdint.h>
#include <string.h>

namespace mt::stock::admin::factory {
enum class Phase:uint8_t {Idle,RetireContacts,ErasePreferences};
// A late mount can appear after initial board startup. Poll at most once
// per second; a permanent error holds the pending intent for recovery.
class EraseRetry {
  bool waiting_=false;
  uint64_t next_=0;
public:
  void begin(){waiting_=true;next_=0;}
  bool waiting()const{return waiting_;}
  template<class Finish> int poll(uint64_t now,Finish finish){
    if(!waiting_||now<next_)return -EAGAIN;
    const int rc=finish();
    if(rc==-EAGAIN||rc==-EBUSY){next_=now+1000;return rc;}
    waiting_=false;return rc;
  }
};
constexpr char Intent[]="mtreset/config";
struct Record {uint8_t bytes[44];};
inline Record record(Phase phase,uint32_t node,const uint8_t key[32]){
  Record r{{'Z','F','C',1,uint8_t(phase),uint8_t(~uint8_t(phase)),0,0}};
  for(unsigned i=0;i<4;++i)r.bytes[i+8]=uint8_t(node>>(8*i));
  memcpy(r.bytes+12,key,32);return r;
}
template<class Store> int inspect(Store &store,uint32_t node,const uint8_t key[32],Phase &phase){
  Record r{};phase=Phase::Idle;
  const int n=store.read(Intent,r.bytes,sizeof(r.bytes));
  if(n==-ENOENT)return 0;
  if(n!=int(sizeof(r)))return n<0?n:-EILSEQ;
  const Phase phases[]={Phase::RetireContacts,Phase::ErasePreferences};
  for(auto p:phases){
    const auto expected=record(p,node,key);
    if(!memcmp(r.bytes,expected.bytes,sizeof(r))){phase=p;return 0;}
  }
  return -EILSEQ;
}
template<class Store> int mark(Store &store,uint32_t node,const uint8_t key[32],Phase phase){
  const auto r=record(phase,node,key);
  const int written=store.write(Intent,r.bytes,sizeof(r.bytes));
  if(written)return written;
  Phase found;const int rc=inspect(store,node,key,found);
  return rc?rc:found==phase?0:-EIO;
}
// Only application preferences are eligible. The cryptographic identity,
// counters, replay history, native Bluetooth bonds and GATT store are separate.
inline constexpr const char *PreferencePairs[]={"mtbase/state","mtbase/nodes","mtmod/state","mtble/state",
    "mtgpio/state","mtcomplete/state","mtextra/state","mtcanned/state","mtringtone/state","mtnotify/state",
    "mtmqtt/state","mtwifi/state","mtsf/state","mtui/state","mtpin/state","mtadmin/state","mtrole/intent",
#if defined(CONFIG_MT_PERSIST_BROADCAST_HISTORY)
    "mttx/history",
#endif
    "mt_hot/meta"};
inline constexpr const char *PreferenceSingles[]={"mtbase/config","mtgames/scores1","mtway/history","mtsat/epoch",
    "mtxsensor/bme680","mtxsensor/nau","mtxsensor/as3935","mtxsensor/sen5","mtxsensor/sen6","mtxsensor/sen5hist","mtxsensor/sen6hist"};
template<class Erase> int erasePreferences(Erase erase){
  for(const char *stem:PreferencePairs){
    char name[20]{};const auto len=strlen(stem);memcpy(name,stem,len);
    for(unsigned i=0;i<2;++i){name[len]=char('0'+i);const int rc=erase(name);if(rc)return rc;}
  }
  for(const char *name:PreferenceSingles){const int rc=erase(name);if(rc)return rc;}
  return 0;
}
template<class Store> int eraseStoredPreferences(Store &store){
  return erasePreferences([&](const char *name){return store.eraseVerified(name);});
}
// These are two boot phases. A restart before the verified phase transition
// repeats only completed, idempotent work and never re-opens RF on mixed state.
template<class Store,class Retire> int retireAndAdvance(Store &store,uint32_t node,const uint8_t key[32],Retire retire){
  Phase phase;const int rc=inspect(store,node,key,phase);
  if(rc||phase!=Phase::RetireContacts)return rc?rc:-EINVAL;
  const int retired=retire();return retired?retired:mark(store,node,key,Phase::ErasePreferences);
}
template<class Store,class RemoveFile> int finish(Store &store,uint32_t node,const uint8_t key[32],RemoveFile removeFile){
  Phase phase;int rc=inspect(store,node,key,phase);
  if(rc||phase!=Phase::ErasePreferences)return rc?rc:-EINVAL;
  rc=removeFile();
  if(!rc)rc=eraseStoredPreferences(store);
  return rc?rc:store.eraseVerified(Intent);
}
}
