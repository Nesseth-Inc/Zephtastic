// SPDX-License-Identifier: GPL-3.0-only
// Read-only synthetic NOR; hash oracle uses the pinned one-shot Monocypher API.
#include "pki/LegacyTailProbe.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
using namespace mt::pki;
struct Nor final:AppendStore {
  std::vector<uint8_t> bytes=std::vector<uint8_t>(LegacyTailProbe::Bytes,0xff);
  size_t reads=0,programs=0,failOffset=LegacyTailProbe::Bytes;int error=-EIO;bool partial=false;
  size_t capacity()const override{return bytes.size();}
  int read(size_t at,uint8_t *out,size_t n)override{
    assert(n==64&&at==reads*64&&at<=bytes.size()&&n<=bytes.size()-at);++reads;
    if(at==failOffset&&!partial)return error;
    memcpy(out,bytes.data()+at,n);return at==failOffset?error:0;
  }
  int program(size_t,const uint8_t *,size_t)override{++programs;return -EACCES;}
  void reset(){std::fill(bytes.begin(),bytes.end(),0xff);reads=0;failOffset=LegacyTailProbe::Bytes;}
};
void put(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
void pass(const char *s){std::printf("PASS %s\n",s);}
void verify(Nor &store,bool blank,bool possible,unsigned nonff){
  store.reads=0;TailProbeResult out;assert(!LegacyTailProbe::inspect(store,out));uint8_t expected[32]{};
  crypto_blake2b(expected,32,store.bytes.data(),store.bytes.size());
  assert(!memcmp(out.digest,expected,32)&&out.blank==blank&&out.possibleJournal==possible&&out.nonFF==nonff);
  assert(store.reads==2688&&!store.programs);
}
void refused(Nor &store,int expected){
  TailProbeResult out;std::fill(std::begin(out.digest),std::end(out.digest),0x5a);
  out.nonFF=9;out.blank=true;out.possibleJournal=true;store.reads=0;
  assert(LegacyTailProbe::inspect(store,out)==expected&&out.nonFF==9&&out.blank&&out.possibleJournal);
  for(auto b:out.digest)assert(b==0x5a);
  assert(!store.programs);
}
int main(){
  Nor n;verify(n,true,false,0);n.bytes.front()=0;n.bytes.back()=0;verify(n,false,false,2);
  pass("complete bounded scan, first/last byte counts and independent one-shot hash");
  for(unsigned prefix=1;prefix<=4;++prefix){n.reset();memcpy(n.bytes.data(),"ZTAH",prefix);verify(n,false,true,prefix);}
  n.reset();n.bytes[0]=uint8_t('Z')|1;verify(n,false,true,1);
  n.reset();n.bytes[0]=0;verify(n,false,false,1);
  pass("partial NOR-compatible magic is conservative and all-FF is not journal evidence");
  n.reset();put(n.bytes.data()+4092,0x54494d43u);verify(n,false,true,4);
  n.bytes[4092]^=1;verify(n,false,false,4);
  for(unsigned index:{0u,1u,255u,256u,2623u}){
    n.reset();put(n.bytes.data()+4096+64*index+60,0x54494d43u);verify(n,false,true,4);
    for(unsigned kind=1;kind<=4;++kind){
      n.reset();auto *p=n.bytes.data()+4096+64*index;p[0]=uint8_t(kind);p[1]=1;p[2]=p[3]=0;put(p+4,index+1);
      verify(n,false,true,8);put(p+4,index+2);verify(n,false,false,8);
    }
  }
  pass("root/event commit and event-header sequence boundaries including last cell");
  n.reset();for(size_t size:{size_t(0),LegacyTailProbe::Bytes-1,LegacyTailProbe::Bytes+1}){
    n.bytes.resize(size,0xff);refused(n,-ENOTSUP);assert(!n.reads);
  }
  n.bytes.resize(LegacyTailProbe::Bytes,0xff);n.reset();
  pass("capacity mismatch rejects before reads and preserves every output member");
  for(size_t at:{size_t(0),size_t(64),size_t(4032),size_t(4096),LegacyTailProbe::Bytes-64})
    for(int error:{-EIO,-EAGAIN,1})for(bool partial:{false,true}){
      n.reset();n.failOffset=at;n.error=error;n.partial=partial;refused(n,error<0?error:-EIO);
      assert(n.reads==at/64+1);
    }
  pass("early/partial/late read errors and unexpected positive errors preserve outputs with zero programming");
}
