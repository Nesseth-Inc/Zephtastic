// SPDX-License-Identifier: GPL-3.0-only
// Native disabled-profile behavior using the existing production NVS fixture.
#define MT_PKI_LEGACY_TAIL_TOOLS_NATIVE_DISABLED 1
#define main pki_storage_nvs_default_main
#include "pki_storage_nvs/cases.cpp"
#undef main

namespace {
struct MemoryAppendStore final:AppendStore {
  std::array<uint8_t,PkiAppendHistory::StoreBytes> bytes;
  unsigned programs=0;
  MemoryAppendStore(){bytes.fill(0xff);}
  size_t capacity()const override{return bytes.size();}
  int read(size_t offset,uint8_t *out,size_t size)override{
    if(!out||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    memcpy(out,bytes.data()+offset,size);return 0;
  }
  int program(size_t offset,const uint8_t *data,size_t size)override{
    if(!data||offset>bytes.size()||size>bytes.size()-offset)return -EINVAL;
    ++programs;
    for(size_t i=0;i<size;++i){if((bytes[offset+i]&data[i])!=data[i])return -EIO;bytes[offset+i]&=data[i];}
    return 0;
  }
};
void disabledBootAndAnchoredFormat3(){
  static_assert(MT_PKI_LEGACY_TAIL_TOOLS_ENABLED==0);
  model_reset(4);Fixture f;f.provision();MemoryAppendStore appendBytes;
  PkiAppendHistory append(appendBytes);PkiStorage storage(f.boot,f.settings,f.context,f.entropy,&append);
  const unsigned writes=model_writes();(void)f.activate(storage);
  assert(storage.state()==PkiStorage::State::Active&&storage.boundTo(f.local.binding.publicKey,f.local.binding.node));
  assert(model_writes()==writes&&f.entropy.calls==0);
  PkiLedger::Status status;assert(storage.ledgerStatus(status)&&status.format==2&&status.active==1);
  assert(!storage.enableLongStorage(f.local.binding));assert(storage.extendedStorage());
  assert(model_writes()>writes&&appendBytes.programs>0);
  Key added(119);PkiPeerPin pin;pin.node=added.binding.node;memcpy(pin.publicKey,added.binding.publicKey,32);
  assert(!storage.addTrustedPeer(f.local.binding,pin));
  assert(storage.active(pin.node,pin.publicKey));assert(storage.close());

  model_remount();PkiAppendHistory reopened(appendBytes);PkiStorage next(f.boot,f.settings,f.context,f.entropy,&reopened);
  assert(next.prepare()==0);assert(!next.activate(f.local.binding.node));assert(next.extendedStorage());
  assert(next.ledgerStatus(status)&&status.format==3&&status.active==2);
  assert(next.active(f.peer.binding.node,f.peer.binding.publicKey)&&next.active(pin.node,pin.publicKey));
  assert(next.close());
  pass("disabled native profile loads existing identity/ledger, explicitly enables and reopens anchored format-3 history, and appends an authorized peer");
}
void disabledEnablePreservesBlankRefusal(){
  model_reset(4);Fixture f;f.provision();MemoryAppendStore appendBytes;appendBytes.bytes.back()=0;
  const auto before=appendBytes.bytes;const unsigned programs=appendBytes.programs,writes=model_writes();
  PkiAppendHistory append(appendBytes);PkiStorage storage(f.boot,f.settings,f.context,f.entropy,&append);
  (void)f.activate(storage);
  assert(storage.enableLongStorage(f.local.binding)==-ENOTEMPTY);
  assert(storage.state()==PkiStorage::State::Active&&!storage.extendedStorage());
  assert(appendBytes.programs==programs&&appendBytes.bytes==before&&model_writes()==writes);
  assert(storage.active(f.peer.binding.node,f.peer.binding.publicKey));
  assert(storage.close());
  pass("disabled native profile retains no-write refusal for a nonblank append region without freezing legacy storage");
}
}
int main(){disabledBootAndAnchoredFormat3();disabledEnablePreservesBlankRefusal();return 0;}
