// SPDX-License-Identifier: GPL-3.0-only
// Uncompiled standalone candidate: real production ledger/journal, synthetic
// public test keys and a NOR fault model only. Root owns compilation/results.
#include "pki/PkiAppendHistory.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace mt::pki;
using Bytes=std::vector<uint8_t>;
static unsigned checks=0,groups=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::abort();}}while(0)
static void pass(const char *s){++groups;std::printf("PASS %s\n",s);}
static uint32_t get(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
struct Key {
  uint8_t scalar[32]{},pub[32]{};uint32_t node=0;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+i*17);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(pub,scalar);node=bench::MtSignature::nodeId(pub);}
  ~Key(){eraseIdentityBytes(scalar,sizeof(scalar));}
};
struct Memory final:mt::SettingsBackend {
  std::map<std::string,Bytes> values;unsigned writes=0;
  int read(const char *key,uint8_t *out,size_t cap)override{auto it=values.find(key);if(it==values.end())return -ENOENT;if(it->second.size()>cap)return -E2BIG;if(!it->second.empty())std::memcpy(out,it->second.data(),it->second.size());return int(it->second.size());}
  int write(const char *key,const uint8_t *in,size_t n)override{++writes;values[key]=Bytes(in,in+n);return 0;}
};
struct Flash final:AppendStore {
  Bytes data=Bytes(PkiAppendHistory::StoreBytes,255);size_t reported=PkiAppendHistory::StoreBytes;
  long budget=-1;unsigned programs=0,reads=0;bool failRead=false;
  PkiAppendHistory *reenter=nullptr;bool checkedReentry=false;
  size_t capacity()const override{return reported;}
  int read(size_t off,uint8_t *out,size_t n)override{
    ++reads;if(failRead)return -EIO;if(off>data.size()||n>data.size()-off)return -EINVAL;
    if(reenter&&!checkedReentry){checkedReentry=true;CHECK(reenter->reserve(0x7fffff01u,true)==-EBUSY);}
    if(n)std::memcpy(out,data.data()+off,n);
    return 0;
  }
  int program(size_t off,const uint8_t *in,size_t n)override{
    ++programs;if((off&3)||(n&3)||off>data.size()||n>data.size()-off)return -EINVAL;
    for(size_t i=0;i<n;++i){if(budget==0)return -EIO;if((data[off+i]&in[i])!=in[i])return -EIO;data[off+i]&=in[i];if(budget>0)--budget;}
    // A full accepted write may still report uncertain failure.
    return budget==0?-EIO:0;
  }
};
struct Baseline {
  Key local{11},a{59},b{107},c{149},d{193};Memory settings;PkiLedger ledger;
  explicit Baseline(bool full=false){
    CHECK(!ledger.load(settings));CHECK(!ledger.initialize(settings,local.pub,local.node,true));
    for(const Key *k:{&a,&b,&c,&d})CHECK(!ledger.addTrustedPeer(settings,k->node,k->pub,true));
    if(full){
      for(unsigned i=0;i<144;++i)CHECK(!ledger.reserve(settings,0x10000001u+i,true));
      unsigned peer=0;for(const Key *k:{&a,&b,&c,&d}){for(unsigned i=0;i<72;++i)CHECK(!ledger.commitAuthenticated(settings,k->node,k->pub,0x20000001u+peer*1024+i,true));++peer;}
    }
  }
};
static AppendAnchor activate(Baseline &b,PkiAppendHistory &h){AppendAnchor a;CHECK(!h.initialize(b.ledger,true,a));CHECK(!h.ready());CHECK(!b.ledger.installAppendAnchor(b.settings,a,true));CHECK(!h.open(b.ledger,a));CHECK(h.ready());return a;}
static void state(const PkiAppendHistory &h,unsigned sent,unsigned received,unsigned stored,unsigned active,uint32_t generation){PkiLedger::Status s;CHECK(h.status(s));CHECK(s.format==3&&s.sent==sent&&s.received==received&&s.stored==stored&&s.active==active&&s.generation==generation);}
static void frozen(PkiAppendHistory &h){CHECK(!h.ready());PkiLedger::Status s;s.generation=123;CHECK(!h.status(s)&&s.generation==123);uint8_t key[32];std::memset(key,77,sizeof(key));CHECK(!h.lookup(2,key)&&key[0]==77);CHECK(h.sent(999));CHECK(h.remaining()==0);}
static void reseal(Flash &f,unsigned index,uint32_t previous){uint8_t *p=f.data.data()+PkiAppendHistory::RootBytes+index*64;put(p+52,previous);put(p+56,crc(p,56));put(p+60,0x54494d43u);}
int main(){
  static_assert(PkiAppendHistory::StoreBytes==172032&&PkiAppendHistory::RootBytes==4096&&PkiAppendHistory::CellBytes==64&&PkiAppendHistory::EventLimit==2624,"Concrete geometry");
  {
    Baseline b;Flash f;PkiAppendHistory h(f);AppendAnchor out;out.generation=99;out.snapshotCrc=98;const auto before=f.data;
    CHECK(h.initialize(b.ledger,false,out)==-EAGAIN&&out.generation==99&&out.snapshotCrc==98&&f.data==before&&f.programs==0);
    CHECK(!h.initialize(b.ledger,true,out));CHECK(!h.ready());
    PkiAppendHistory premature(f);CHECK(premature.open(b.ledger,out)<0);frozen(premature);
    CHECK(!b.ledger.installAppendAnchor(b.settings,out,true));CHECK(!h.open(b.ledger,out));state(h,0,0,4,4,out.generation);
    Flash lost;PkiAppendHistory recreate(lost);AppendAnchor unchanged;unchanged.generation=71;CHECK(recreate.initialize(b.ledger,true,unchanged)<0&&lost.programs==0&&unchanged.generation==71);
    CHECK(h.open(b.ledger,out)==-EACCES);pass("selector-last readiness, quiescence and selected missing-root recreation refusal");
  }
  {
    Baseline b;Flash f;f.data.back()=0xfe;const auto before=f.data;PkiAppendHistory h(f);AppendAnchor a;a.generation=123;
    CHECK(h.initialize(b.ledger,true,a)==-ENOTEMPTY&&a.generation==123&&f.data==before&&f.programs==0);frozen(h);
    Flash wrong;wrong.reported-=4096;PkiAppendHistory x(wrong);CHECK(x.initialize(b.ledger,true,a)==-EXDEV&&wrong.programs==0);pass("entire region blankness and exact42page geometry before writes");
  }
  {
    Baseline b(true);Flash f;PkiAppendHistory h(f);AppendAnchor a=activate(b,h);const auto nvs=b.settings.values;
    for(unsigned i=0;i<144;++i)CHECK(h.sent(0x10000001u+i));
    CHECK(h.reserve(0x10000001u,true)==-EALREADY);CHECK(!h.reserve(0x70000101u,true));
    unsigned p=0;for(const Key *k:{&b.a,&b.b,&b.c,&b.d}){for(unsigned i=0;i<72;++i)CHECK(h.seen(k->node,k->pub,0x20000001u+p*1024+i)==ReplayCheck::Seen);CHECK(h.seen(k->node,k->pub,0x71000101u)==ReplayCheck::Fresh);CHECK(!h.commitAuthenticated(k->node,k->pub,0x71000101u,true));++p;}
    CHECK(h.commitAuthenticated(b.a.node,b.a.pub,0x71000101u,true)==-EALREADY);state(h,145,292,4,4,a.generation+5);
    PkiAppendHistory restart(f);CHECK(!restart.open(b.ledger,a));CHECK(restart.sent(0x70000101u));CHECK(restart.seen(b.a.node,b.a.pub,0x71000101u)==ReplayCheck::Seen);CHECK(b.settings.values==nvs);pass("all144TX and4x72RX baseline IDs survive and extend without rewriting NVS");
  }
  {
    Baseline b;Flash f;PkiAppendHistory h(f);AppendAnchor a=activate(b,h);Key next(227);unsigned writes=f.programs;
    CHECK(h.addTrustedPeer(next.node,next.pub,false)==-EAGAIN&&f.programs==writes);
    CHECK(h.addTrustedPeer(next.node^1u,next.pub,true)==-EINVAL);CHECK(h.addTrustedPeer(b.local.node,b.local.pub,true)==-EINVAL);
    CHECK(!h.revokeTrustedPeer(b.a.node,b.a.pub,true));CHECK(h.revoked(b.a.node));CHECK(!h.active(b.a.node,b.a.pub));CHECK(h.addTrustedPeer(b.a.node,b.a.pub,true)==-EEXIST);
    CHECK(!h.addTrustedPeer(next.node,next.pub,true));CHECK(h.active(next.node,next.pub));CHECK(!h.commitAuthenticated(next.node,next.pub,1234,true));
    CHECK(!h.revokeTrustedPeer(next.node,next.pub,true));writes=f.programs;CHECK(!h.revokeTrustedPeer(next.node,next.pub,false)&&f.programs==writes);
    for(unsigned seed:{31u,71u,113u,157u,211u}){Key k(seed);CHECK(!h.addTrustedPeer(k.node,k.pub,true));CHECK(!h.revokeTrustedPeer(k.node,k.pub,true));CHECK(h.addTrustedPeer(k.node,k.pub,true)==-EEXIST);CHECK(h.addTrustedPeer(k.node+1u,k.pub,true)==-EINVAL);}
    PkiLedger::Status s;CHECK(h.status(s)&&s.stored==10&&s.active==3);PkiAppendHistory restart(f);CHECK(!restart.open(b.ledger,a));CHECK(restart.revoked(next.node)&&restart.revoked(b.a.node));CHECK(restart.addTrustedPeer(next.node,next.pub,true)==-EEXIST);CHECK(restart.seen(next.node,next.pub,1234)==ReplayCheck::UnknownPeer);pass("active4 versus lifetime>4, immutable domain tags, old node/key refusal and durable tombstones");
  }
  {
    Baseline b;Flash f;PkiAppendHistory h(f);AppendAnchor a=activate(b,h);
    for(unsigned i=0;i<PkiAppendHistory::EventLimit;++i)CHECK(!h.reserve(0x40000001u+i,true));
    CHECK(h.remaining()==0&&h.reserve(0x50000001u,true)==-ENOSPC&&h.reserve(0x40000001u,true)==-EALREADY);state(h,2624,0,4,4,a.generation+2624);
    PkiAppendHistory restart(f);CHECK(!restart.open(b.ledger,a));CHECK(restart.remaining()==0&&restart.sent(0x40000001u)&&restart.sent(0x40000001u+2623));CHECK(restart.seen(b.a.node,b.a.pub,77)==ReplayCheck::Full);pass("all2624 cells, final boundary and exact saturation across restart without GC");
  }
  {
    Baseline b;const Bytes empty(PkiAppendHistory::StoreBytes,255);
    for(unsigned cut=0;cut<4096;++cut){Flash f;f.budget=cut;PkiAppendHistory h(f);AppendAnchor out;out.generation=91;CHECK(h.initialize(b.ledger,true,out)<0&&out.generation==91&&!h.ready());CHECK(!std::memcmp(f.data.data()+4096,empty.data()+4096,f.data.size()-4096));}
    pass("every root write byte cut preserves outputs, remains unready and never touches event region");
  }
  {
    Baseline b;Flash source;PkiAppendHistory origin(source);AppendAnchor a=activate(b,origin);
    for(unsigned cut=0;cut<=64;++cut){Flash f=source;PkiAppendHistory h(f);CHECK(!h.open(b.ledger,a));f.budget=cut;CHECK(h.reserve(0x66000001u,true)<0);frozen(h);f.budget=-1;PkiAppendHistory restart(f);const int rc=restart.open(b.ledger,a);if(cut==0)CHECK(!rc&&!restart.sent(0x66000001u));else if(cut==64)CHECK(!rc&&restart.sent(0x66000001u));else CHECK(rc<0&&!restart.ready());}
    pass("every cell byte cut, uncertain complete commit and no lost burned IDs on restart");
  }
  {
    Baseline b;Flash source;PkiAppendHistory origin(source);AppendAnchor a=activate(b,origin);CHECK(!origin.reserve(1001,true));CHECK(!origin.reserve(1002,true));
    for(unsigned kind=0;kind<6;++kind){Flash f=source;uint8_t *one=f.data.data()+4096,*two=one+64;
      if(kind==0)f.data[68]=1;
      if(kind==1)one[60]^=1;
      if(kind==2){two[0]=99;reseal(f,1,get(one+56));}
      if(kind==3){put(two+16,1001);reseal(f,1,get(one+56));}
      if(kind==4){std::memset(one,255,64);}
      if(kind==5)f.data.back()=0;
      const auto before=f.data;PkiAppendHistory h(f);CHECK(h.open(b.ledger,a)<0);frozen(h);CHECK(f.data==before&&f.programs==source.programs);
    }
    AppendAnchor bad=a;bad.snapshotCrc^=1;PkiAppendHistory wrong(source);CHECK(wrong.open(b.ledger,bad)<0);pass("root/commit/unknown/duplicate/gap/tail/anchor corruption freezes without repair");
  }
  {
    Baseline b;Flash f;PkiAppendHistory h(f);activate(b,h);const auto before=f.data;unsigned writes=f.programs;
    CHECK(h.reserve(123,false)==-EAGAIN&&h.commitAuthenticated(b.a.node,b.a.pub,124,false)==-EAGAIN&&f.programs==writes&&f.data==before);
    uint32_t ids[4]={9,8,7,6};size_t count=99;CHECK(!h.activePeers(ids,3,count)&&count==99&&ids[0]==9);CHECK(h.activePeers(ids,4,count)&&count==4);
    f.reenter=&h;CHECK(!h.reserve(125,true)&&f.checkedReentry);f.reenter=nullptr;f.failRead=true;CHECK(h.reserve(126,true)<0);frozen(h);CHECK(h.close()&&!h.ready()&&h.reserve(127,true)==-EACCES);pass("reentry, quiescence, no failed-output mutation, read failure and terminal close");
  }
  std::printf("{\"PASS\":true,\"groups\":%u,\"assertions\":%u,\"cells\":2624,\"store_bytes\":172032,\"hardware\":false}\n",groups,checks);
}
