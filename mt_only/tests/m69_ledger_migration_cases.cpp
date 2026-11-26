// SPDX-License-Identifier: GPL-3.0-only
// Independent M69 format/migration fixtures. Synthetic public test keys only.
#include "pki/PkiLedger.h"
#include "MtSignature.h"
#include "monocypher.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace mt::pki;
using Bytes = std::vector<uint8_t>;
static constexpr unsigned NewTx=144,NewRx=72;
static constexpr size_t NewRecord=1944;
static unsigned checks=0, groups=0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while (0)
static const char *keys[]={"mtpki/ledger0","mtpki/ledger1"};
static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned b=0;b<8;++b)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
static void seal(Bytes &b){put32(b.data()+b.size()-4,crc(b.data(),b.size()-4));}
static void pass(const char *name){++groups;std::printf("PASS %s\n",name);}
struct Key {
  uint8_t scalar[32]{},pub[32]{};uint32_t node=0;
  explicit Key(unsigned seed){for(unsigned i=0;i<32;++i)scalar[i]=uint8_t(seed+i*17);scalar[0]&=248;scalar[31]=(scalar[31]&127)|64;crypto_x25519_public_key(pub,scalar);node=bench::MtSignature::nodeId(pub);}
  ~Key(){eraseIdentityBytes(scalar,sizeof(scalar));}
};
struct History {
  Key local{11};std::array<Key,4> peers{{Key(59),Key(107),Key(149),Key(193)}};
  std::vector<uint32_t> tx;std::array<std::vector<uint32_t>,4> rx;
  std::array<bool,4> revoked{{false,false,false,false}};unsigned peerCount=4;
  uint32_t generation()const{uint32_t n=1+uint32_t(tx.size())+peerCount;for(unsigned i=0;i<peerCount;++i)n+=uint32_t(rx[i].size())+unsigned(revoked[i]);return n;}
};
// Reference wire encoder from the stated v1/v2 contract, not production encode.
static Bytes encodeLayout(const History &h,unsigned version,unsigned tx,unsigned rx){
  CHECK(version==1||version==2);
  const size_t offset=52+tx*4,peerSize=40+rx*4;Bytes b(offset+4*peerSize+4,0);
  CHECK(h.tx.size()<=tx&&h.peerCount<=4);std::memcpy(b.data(),"ZTPL",4);b[4]=uint8_t(version);b[5]=uint8_t(tx);b[6]=4;b[7]=uint8_t(rx);
  put32(b.data()+8,h.generation());put32(b.data()+12,h.local.node);std::memcpy(b.data()+16,h.local.pub,32);b[48]=uint8_t(h.tx.size());b[49]=uint8_t(h.peerCount);
  for(size_t i=0;i<h.tx.size();++i)put32(b.data()+52+4*i,h.tx[i]);
  for(unsigned i=0;i<h.peerCount;++i){CHECK(h.rx[i].size()<=rx);auto *p=b.data()+offset+i*peerSize;put32(p,h.peers[i].node);std::memcpy(p+4,h.peers[i].pub,32);p[36]=uint8_t(h.rx[i].size());p[37]=uint8_t(h.revoked[i]);for(size_t j=0;j<h.rx[i].size();++j)put32(p+40+4*j,h.rx[i][j]);}
  seal(b);return b;
}
static Bytes encode(const History &h,unsigned version){return encodeLayout(h,version,version==1?128:NewTx,version==1?64:NewRx);}
struct Memory:mt::SettingsBackend {
  std::map<std::string,Bytes> values;unsigned reads=0,writes=0;int writeError=0,cut=-1,forcedRead=-1;bool store=true;
  std::string lastKey;size_t lastSize=0;
  int read(const char *name,uint8_t *out,size_t cap)override{++reads;auto i=values.find(name);if(i==values.end())return -ENOENT;if(forcedRead>=0)return forcedRead;if(i->second.size()>cap)return -E2BIG;if(!i->second.empty())std::memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
  int write(const char *name,const uint8_t *p,size_t n)override{++writes;lastKey=name;lastSize=n;CHECK(n==NewRecord);if(store){const size_t count=cut<0?n:size_t(cut);CHECK(count<=n);values[name]=Bytes(p,p+count);}return writeError;}
};
static Memory legacy(const History &h){CHECK(!h.tx.empty());History old=h;old.tx.pop_back();Memory m;m.values[keys[0]]=encode(old,1);m.values[keys[1]]=encode(h,1);return m;}
static void preserved(PkiLedger &l,const History &h){
  CHECK(l.ready()&&l.boundTo(h.local.pub,h.local.node));for(auto id:h.tx)CHECK(l.sent(id));
  for(unsigned i=0;i<h.peerCount;++i){CHECK(l.revoked(h.peers[i].node)==h.revoked[i]);CHECK(l.active(h.peers[i].node,h.peers[i].pub)==!h.revoked[i]);for(auto id:h.rx[i])CHECK(l.seen(h.peers[i].node,h.peers[i].pub,id)==(h.revoked[i]?ReplayCheck::UnknownPeer:ReplayCheck::Seen));}
}
static void statusMatches(PkiLedger &l,const History &h,unsigned physicalVersion){
  unsigned received=0,active=0;for(unsigned i=0;i<h.peerCount;++i){received+=unsigned(h.rx[i].size());active+=unsigned(!h.revoked[i]);}
  PkiLedger::Status out;CHECK(l.status(out));CHECK(out.format==physicalVersion&&out.generation==h.generation());
  CHECK(out.sent==h.tx.size()&&out.received==received&&out.stored==h.peerCount&&out.active==active);
}
static void statusRefused(PkiLedger &l){
  PkiLedger::Status out;out.generation=0xabcdef01;out.sent=333;out.received=444;out.format=99;out.stored=88;out.active=77;
  CHECK(!l.status(out));CHECK(out.generation==0xabcdef01&&out.sent==333&&out.received==444&&out.format==99&&out.stored==88&&out.active==77);
}
static void rejected(Memory m){PkiLedger l;const auto before=m.values;CHECK(l.load(m)<0);CHECK(!l.ready()&&m.writes==0&&m.values==before);CHECK(l.reserve(m,123,true)==-EACCES);statusRefused(l);}
int main(){
  static_assert(PkiLedger::LegacyRecordSize==1752,"independent legacy format");
  static_assert(PkiLedger::RecordSize==NewRecord&&PkiLedger::SendLimit==NewTx&&PkiLedger::ReceiveLimit==NewRx&&PkiLedger::PeerLimit==4,"independent144/72/1944 candidate contract");
  History h;h.tx={0xf0000011u,7,0x91000003u};h.rx[0]={19,0xe0000001u};h.rx[1]={87};h.rx[2]={0x80000002u};h.revoked[2]=true;
  {
    Memory m=legacy(h);const auto before=m.values;PkiLedger l;CHECK(!l.load(m));preserved(l,h);statusMatches(l,h,1);CHECK(m.writes==0&&m.values==before);
    CHECK(!l.reserve(m,1234567,true));h.tx.push_back(1234567);statusMatches(l,h,2);CHECK(m.writes==1&&m.lastKey==keys[0]&&m.lastSize==NewRecord);CHECK(m.values[keys[0]]==encode(h,2));
    PkiLedger mixed;CHECK(!mixed.load(m));preserved(mixed,h);statusMatches(mixed,h,2);CHECK(m.writes==1);
    CHECK(!mixed.commitAuthenticated(m,h.peers[0].node,h.peers[0].pub,0xa0000001u,true));h.rx[0].push_back(0xa0000001u);CHECK(m.values[keys[1]]==encode(h,2));
    PkiLedger again;CHECK(!again.load(m));preserved(again,h);statusMatches(again,h,2);CHECK(m.writes==2);pass("v1 read-only normalization, exact alternate v2 write, mixed/v2 reload, histories/tombstones and selected physical-format status preserved");
  }
  h.tx.pop_back();h.rx[0].pop_back();
  for(unsigned mode=0;mode<12;++mode){
    Memory m=legacy(h);auto &b=m.values[keys[1]];
    if(mode==0)b[20]^=1; // Original CRC must be checked before normalization.
    if(mode==1){b[4]=3;seal(b);}if(mode==2){b[5]=255;seal(b);}if(mode==3){b[7]=128;seal(b);}
    if(mode==4){b[51]=1;seal(b);}if(mode==5){put32(b.data()+52+h.tx.size()*4,900);seal(b);}
    if(mode==6){b.resize(1751);}if(mode==7){b.push_back(0);}
    if(mode==8){put32(b.data()+8,get32(b.data()+8)+1);seal(b);}
    if(mode==9){put32(b.data()+52,0x1234);seal(b);} // Valid CRC, rewritten TX prefix.
    if(mode==10){put32(b.data()+564+40,777);seal(b);} // Valid CRC, rewritten RX prefix.
    if(mode==11){m.values.erase(keys[0]);} // Lone generation >1 is not history.
    rejected(m);
  }
  pass("legacy CRC, format, lengths, unused bytes, generation, prefix and missing-slot corruptions refuse without boot writes");
  {
    Memory m=legacy(h);History next=h;next.tx.push_back(888);m.values[keys[0]]=encode(next,2);rejected([&]{Memory x=m;x.values[keys[0]][4]=3;seal(x.values[keys[0]]);return x;}());
    m.values[keys[1]]=encode(h,2);next.tx.push_back(889);m.values[keys[0]]=encode(next,2);rejected(m); // Gap of two operations.
    m=legacy(h);History unrevoked=h;unrevoked.revoked[2]=false;unrevoked.tx.push_back(888);unrevoked.tx.push_back(889);m.values[keys[0]]=encode(unrevoked,2);rejected(m);
    pass("mixed/v2 unknown format, generation gap and un-revocation refuse");
  }
  {
    History next=h;next.tx.push_back(4444444);
    for(unsigned order=0;order<2;++order){
      // A newer legacy record cannot follow a committed v2 record, even when
      // its normalized logical history would otherwise be a valid extension.
      Memory down;down.values[keys[order]]=encode(h,2);down.values[keys[1-order]]=encode(next,1);rejected(down);
      // Equal normalized generation/content cannot erase the wire transition.
      Memory same;same.values[keys[order]]=encode(h,1);same.values[keys[1-order]]=encode(h,2);rejected(same);
      // The permitted direction remains valid in either physical slot order.
      Memory forward;forward.values[keys[order]]=encode(h,1);forward.values[keys[1-order]]=encode(next,2);
      const auto before=forward.values;PkiLedger l;CHECK(!l.load(forward));preserved(l,next);statusMatches(l,next,2);CHECK(forward.writes==0&&forward.values==before);
    }
    pass("both slot orders reject reverse-version and equal-generation mixed records, admit only forward contiguous migration");
  }
  {
    for(unsigned order=0;order<2;++order){Memory m=legacy(h);m.values[keys[order]]=encodeLayout(h,2,255,128);CHECK(m.values[keys[order]].size()==3284);rejected(m);}
    pass("uninstalled experimental255/128/3284 format2 is rejected in both physical slot orders");
  }
  {
    const Memory base=legacy(h);const Bytes previous=base.values.at(keys[1]);
    for(unsigned cut=0;cut<=NewRecord;++cut){Memory m=base;PkiLedger l;CHECK(!l.load(m));m.cut=int(cut);m.writeError=-EIO;CHECK(l.reserve(m,555,true)<0);CHECK(!l.ready()&&m.values[keys[1]]==previous);statusRefused(l);PkiLedger restart;const int rc=restart.load(m);CHECK(cut==NewRecord?rc==0:rc<0);if(!rc)CHECK(restart.sent(555));}
    Memory m=base;PkiLedger l;CHECK(!l.load(m));m.store=false;CHECK(l.reserve(m,556,true)<0&&!l.ready());PkiLedger old;CHECK(!old.load(m)&&!old.sent(556));
    m=base;PkiLedger uncertain;CHECK(!uncertain.load(m));m.forcedRead=1752;CHECK(uncertain.reserve(m,557,true)<0&&!uncertain.ready());m.forcedRead=-1;PkiLedger committed;CHECK(!committed.load(m)&&committed.sent(557));
    pass("every first-v2 write byte cut and exact-readback failure freeze; committed IDs remain burned on restart");
  }
  {
    History full;for(unsigned i=0;i<128;++i)full.tx.push_back(0x10000001u+i);for(unsigned p=0;p<4;++p)for(unsigned i=0;i<64;++i)full.rx[p].push_back(0x20000001u+p*1024+i);
    Memory m=legacy(full);PkiLedger l;CHECK(!l.load(m));preserved(l,full);statusMatches(l,full,1);
    for(unsigned i=128;i<NewTx;++i){CHECK(!l.reserve(m,0x10000001u+i,true));full.tx.push_back(0x10000001u+i);}
    CHECK(l.reserve(m,0x10000001u+NewTx,true)==-ENOSPC);CHECK(l.reserve(m,full.tx[0],true)==-EALREADY);
    for(unsigned p=0;p<4;++p){for(unsigned i=64;i<NewRx;++i){CHECK(!l.commitAuthenticated(m,full.peers[p].node,full.peers[p].pub,0x20000001u+p*1024+i,true));full.rx[p].push_back(0x20000001u+p*1024+i);}CHECK(l.seen(full.peers[p].node,full.peers[p].pub,0x70000001u)==ReplayCheck::Full);}
    PkiLedger reload;CHECK(!reload.load(m));preserved(reload,full);statusMatches(reload,full,2);CHECK(reload.reserve(m,0x10000001u+NewTx,true)==-ENOSPC);CHECK(reload.reserve(m,full.tx.back(),true)==-EALREADY);
    for(unsigned p=0;p<4;++p)CHECK(reload.commitAuthenticated(m,full.peers[p].node,full.peers[p].pub,0x70000001u,true)==-ENOSPC);
    pass("old full128/64 history expands through144/72 for all4 pins and remains exact across reload; next IDs fail closed");
  }
  std::printf("{\"PASS\":true,\"groups\":%u,\"assertions\":%u,\"legacy_bytes\":1752,\"v2_bytes\":%zu,\"first_write_byte_cuts\":%zu,\"hardware\":false}\n",groups,checks,NewRecord,NewRecord+1);
}
