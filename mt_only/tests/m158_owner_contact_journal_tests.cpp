// SPDX-License-Identifier: GPL-3.0-only
// Owner-local SharedContact policy through the production NodeCache/NodeJournal.
#include "NodeJournal.h"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using Cache=mt::NodeCache;
using Journal=mt::NodeJournal;
using Bytes=std::vector<uint8_t>;
constexpr uint32_t Self=0x12121212u,Peer=0x10203040u;

struct Memory final:mt::SettingsBackend {
  std::map<std::string,Bytes> rows;
  unsigned writes=0;
  int writeError=0;
  bool failReadback=false;
  int read(const char *key,uint8_t *out,size_t cap)override {
    if(failReadback&&writes)return -EIO;
    auto it=rows.find(key);if(it==rows.end())return -ENOENT;
    if(it->second.size()>cap)return -E2BIG;
    memcpy(out,it->second.data(),it->second.size());return int(it->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t n)override {
    ++writes;if(writeError)return writeError;rows[key]=Bytes(data,data+n);return 0;
  }
};

Cache::Entry contact(uint32_t id,const char *name="Owner contact",unsigned keyByte=0x42,unsigned keySize=32) {
  Cache::Entry e;e.identity(id,127,1);
  assert(Cache::Entry::text(e.bytes+Cache::Entry::Name,40,name,strlen(name)+1));
  assert(Cache::Entry::text(e.bytes+Cache::Entry::Short,5,"OWN",4));
  e.bytes[Cache::Entry::KeyLength]=uint8_t(keySize);
  for(unsigned i=0;i<keySize;++i)e.bytes[Cache::Entry::Key+i]=uint8_t(keyByte+i);
  e.bytes[Cache::Entry::Flags]=1;
  return e;
}
void seal(Bytes &b) {
  uint32_t c=UINT32_MAX;
  for(size_t i=0;i+4<b.size();++i){c^=b[i];for(unsigned bit=0;bit<8;++bit)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
  Cache::Entry::put32(b.data()+b.size()-4,~c);
}
bool sameBytes(const Cache::Entry *a,const Cache::Entry *b) {
  return a&&b&&memcmp(a->bytes,b->bytes,Cache::Entry::Size)==0;
}
void persist(Memory &b,Cache &c,Journal &j,const Cache::Entry &incoming,bool ignore=false,bool manual=false) {
  assert(j.ownerContact(b,c,incoming,ignore,manual,10,true)==0);
}
}

int main() {
  // New contacts default to favorite; preview is read-only and does not write.
  {
    Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
    Cache::Entry resolved{};const Cache before=c;const auto incoming=contact(Peer);
    assert(!j.previewOwnerContact(c,incoming,false,false,resolved));
    assert(resolved.id()==Peer&&resolved.favorite()&&!resolved.ignored());
    assert(!c.count()&&!c.dirty()&&!b.writes);
    persist(b,c,j,incoming);
    assert(c.count()==1&&c.get(0)->favorite()&&j.generation()==1&&b.writes==1);
    assert(!before.count());
  }

  // Keyless updates preserve a saved key. Ignore clears favorite but keeps that key;
  // a later false-ignore update does not silently remove the ignore policy.
  {
    Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
    persist(b,c,j,contact(Peer,"Saved key"));
    assert(c.get(0)->favorite());
    persist(b,c,j,contact(Peer,"Keyless update",0,0),true,false);
    assert(c.get(0)->ignored()&&!c.get(0)->favorite());
    assert(c.get(0)->bytes[Cache::Entry::KeyLength]==32&&c.get(0)->bytes[Cache::Entry::Key]==0x42);
    persist(b,c,j,contact(Peer,"False ignore",0,0),false,false);
    assert(c.get(0)->ignored()&&c.get(0)->favorite());
    assert(c.get(0)->bytes[Cache::Entry::KeyLength]==32&&c.get(0)->bytes[Cache::Entry::Key]==0x42);
  }

  // Manual verification prevents a nonmanual key replacement. An explicit
  // manual owner update may replace it, and the bit survives a cold journal load.
  {
    Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
    persist(b,c,j,contact(Peer,"Verified key",0x42),false,true);
    assert(c.get(0)->manuallyVerified());
    assert(b.rows[j.activeSlot()==0?"mtbase/nodes0":"mtbase/nodes1"][3]==4);
    persist(b,c,j,contact(Peer,"Keyless verified update",0,0),false,false);
    assert(c.get(0)->manuallyVerified()&&c.get(0)->bytes[Cache::Entry::Key]==0x42);
    const auto old=*c.get(0);const unsigned writes=b.writes;
    assert(j.ownerContact(b,c,contact(Peer,"Unapproved replacement",0x63),false,false,20,true)==-EACCES);
    assert(b.writes==writes&&sameBytes(c.get(0),&old));
    persist(b,c,j,contact(Peer,"Approved replacement",0x63),false,true);
    assert(c.get(0)->manuallyVerified()&&c.get(0)->bytes[Cache::Entry::Key]==0x63);
    Cache reopened;Journal reboot;assert(!reboot.load(b,reopened,Self));
    assert(reopened.get(0)->manuallyVerified()&&reopened.get(0)->bytes[Cache::Entry::Key]==0x63);
  }

  // At protected capacity, a new owner contact remains admissible but falls back
  // to an unprotected cache row instead of exceeding the protected limit.
  {
    Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
    for(unsigned i=0;i<Cache::ProtectedLimit;++i)
      persist(b,c,j,contact(0x20000000u+i,"Protected",0x40+i),false,true);
    for(unsigned i=0;i<2;++i)assert(c.accept(contact(0x30000000u+i,"Unprotected"),Self,1,0)>=0);
    assert(c.count()==Cache::Capacity&&c.protectedCount()==Cache::ProtectedLimit);
    const unsigned writes=b.writes;Cache::Entry resolved{};
    assert(!j.previewOwnerContact(c,contact(Peer,"Fallback"),false,false,resolved));
    assert(resolved.id()==Peer&&!resolved.favorite()&&!resolved.manuallyVerified());
    assert(c.count()==Cache::Capacity&&c.find(Peer)<0&&b.writes==writes);
    persist(b,c,j,contact(Peer,"Fallback"));
    assert(c.count()==Cache::Capacity&&c.protectedCount()==Cache::ProtectedLimit);
    assert(c.find(Peer)>=0&&!c.get(unsigned(c.find(Peer)))->protectedNode());
  }

  // Failed write and failed readback freeze the journal/cache without publishing
  // the staged owner candidate into the live cache.
  for(unsigned mode=0;mode<2;++mode) {
    Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
    const auto old=contact(0x40000000u,"Existing",0x44);assert(c.accept(old,Self,1,0)==0);
    const auto before=*c.get(0);const auto incoming=contact(Peer,"Not published");
    if(mode==0)b.writeError=-EIO;else b.failReadback=true;
    assert(j.ownerContact(b,c,incoming,false,false,30,true)<0);
    assert(c.count()==1&&c.find(Peer)<0&&sameBytes(c.get(0),&before));
    assert(!c.ready()&&!j.ready());
  }

  // Version 3 remains readable for legacy policy bits; v3 cannot claim the
  // version-4 ManualVerified bit even with a freshly valid checksum.
  {
    Memory b;Cache c;Journal j;assert(!j.load(b,c,Self));
    assert(c.accept(contact(Peer,"Legacy"),Self,1,0)==0);
    assert(j.flush(b,c,0,true,true)==1);
    auto &raw=b.rows["mtbase/nodes0"];assert(raw[3]==3);
    Cache loaded;Journal legacy;assert(!legacy.load(b,loaded,Self)&&loaded.count()==1);
    raw[16+Cache::Entry::LocalFlags]|=Cache::Entry::ManualVerified;seal(raw);
    Cache rejected;Journal invalid;assert(invalid.load(b,rejected,Self)==-EILSEQ&&!rejected.ready());
  }

  return 0;
}
