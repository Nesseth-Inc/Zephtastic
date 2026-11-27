// SPDX-License-Identifier: GPL-3.0-only
#include "modules/CannedMessageJournal.h"
#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>

using namespace mt;
using namespace mt::modules;
using Bytes=std::vector<uint8_t>;

struct Backend final:SettingsBackend {
  enum class Fault {None,ReadError,BeforeWrite,CommittedError,TornError,TornSuccess,ReadbackError,ReadbackShort,ReadbackLong,ReadbackDifferent};
  std::unordered_map<std::string,Bytes> rows;
  std::vector<std::string> reads,writes;
  Fault fault=Fault::None;
  bool afterWrite=false;
  int read(const char *key,uint8_t *out,size_t capacity)override {
    reads.emplace_back(key);
    if(fault==Fault::ReadError||(afterWrite&&fault==Fault::ReadbackError))return -EIO;
    const auto it=rows.find(key);if(it==rows.end())return -ENOENT;
    if(it->second.size()>capacity)return -E2BIG;
    const size_t n=it->second.size();if(n)memcpy(out,it->second.data(),n);
    if(afterWrite&&fault==Fault::ReadbackLong){
      if(n==capacity)return -E2BIG;
      out[n]=0;return int(n+1);
    }
    if(afterWrite&&fault==Fault::ReadbackDifferent&&n)out[n-1]^=1;
    return int(afterWrite&&fault==Fault::ReadbackShort&&n?n-1:n);
  }
  int write(const char *key,const uint8_t *data,size_t size)override {
    writes.emplace_back(key);afterWrite=true;
    if(fault==Fault::BeforeWrite)return -EIO;
    rows[key]=Bytes(data,data+size);
    if(fault==Fault::TornError||fault==Fault::TornSuccess)rows[key].resize(13);
    return fault==Fault::CommittedError||fault==Fault::TornError?-EIO:0;
  }
};

static unsigned groups=0;
static void pass(const char *label){++groups;std::printf("PASS %s\n",label);}
static CannedMessageSettings settings(const char *value){
  CannedMessageSettings out;const size_t n=strlen(value);
  assert(out.assign(value,n)==CannedMessageSettings::AssignResult::Changed&&out.valid());return out;
}
static uint32_t crc32(const uint8_t *p,size_t n){
  uint32_t c=UINT32_MAX;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(c&1)?(c>>1)^0xedb88320u:c>>1;}return ~c;
}
static uint16_t get16(const uint8_t *p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
static void seal(Bytes &b){assert(b.size()>=16);Config::put32(b.data()+b.size()-4,crc32(b.data(),b.size()-4));}
static Bytes legacyRecord(const CannedMessageSettings &settings,uint32_t generation){
  constexpr size_t inner=208,record=224;Bytes b(record);b[0]='Z';b[1]='T';b[2]='C';b[3]=1;
  Config::put32(b.data()+4,generation);b[8]=uint8_t(inner);b[9]=uint8_t(inner>>8);
  uint8_t *p=b.data()+12;p[0]='Z';p[1]='T';p[2]='C';p[3]=1;
  memcpy(p+4,settings.messages,sizeof(settings.messages));
  const auto innerCrc=Config::checksum(p,206);p[206]=uint8_t(innerCrc);p[207]=uint8_t(innerCrc>>8);
  seal(b);return b;
}
static Backend two(){
  Backend b;CannedMessageJournal j;assert(!j.load(b));assert(!j.save(b,settings("one|two")));assert(!j.save(b,settings("new|list")));b.afterWrite=false;return b;
}
static void expectInvalidOnly(const Bytes &record){
  Backend b;b.rows["mtcanned/state0"]=record;const auto before=b.rows;
  CannedMessageJournal j;assert(j.load(b)==-EINVAL&&!j.ready()&&j.state()==CannedMessageJournal::State::Invalid);
  assert(j.config().same(CannedMessageSettings{})&&b.writes.empty()&&b.rows==before);
}

int main(){
  {
    CannedMessageSettings s;
    assert(s.valid()&&s.length()==strlen("Hi|Bye|Yes|No|Ok")&&
      !memcmp(s.messages,"Hi|Bye|Yes|No|Ok",sizeof("Hi|Bye|Yes|No|Ok")));
    const auto before=s;
    assert(s.assign("",0)==CannedMessageSettings::AssignResult::IgnoredEmpty&&s.same(before));
    assert(s.assign(nullptr,4)==CannedMessageSettings::AssignResult::Invalid&&s.same(before));
    const char embedded[]={'a',0,'b'};
    assert(s.assign(embedded,sizeof(embedded))==CannedMessageSettings::AssignResult::Invalid&&s.same(before));
    std::string max(200,'x');
    assert(s.assign(max.data(),max.size())==CannedMessageSettings::AssignResult::Changed&&s.length()==200&&s.valid());
    max.push_back('x');assert(s.assign(max.data(),max.size())==CannedMessageSettings::AssignResult::Invalid&&s.length()==200);
    const char utf8[]="Māori|東京|🙂";
    assert(s.assign(utf8,sizeof(utf8)-1)==CannedMessageSettings::AssignResult::Changed&&
      s.length()==sizeof(utf8)-1&&!memcmp(s.messages,utf8,sizeof(utf8)));
    pass("stock default, empty setter no-op, exact 200-byte limit, embedded-NUL rejection, and UTF-8 byte preservation");
  }
  {
    Backend b;CannedMessageJournal j;
    assert(!j.ready()&&j.save(b,settings("x"))==-EACCES&&b.reads.empty()&&b.writes.empty());
    assert(!j.load(b)&&j.ready()&&j.state()==CannedMessageJournal::State::Fresh&&j.generation()==0&&j.activeSlot()==-1);
    assert(j.config().same(CannedMessageSettings{})&&b.rows.empty()&&b.writes.empty());
    assert(b.reads==std::vector<std::string>({"mtcanned/state0","mtcanned/state1"}));
    pass("absent slots produce stock RAM defaults without creating persistent state");
  }
  {
    Backend b;CannedMessageJournal j;assert(!j.load(b));
    auto first=settings("alpha|βeta");assert(!j.save(b,first));
    assert(j.generation()==1&&j.activeSlot()==0&&b.writes==std::vector<std::string>({"mtcanned/state0"}));
    const auto &record=b.rows.at("mtcanned/state0");
    assert(record.size()==16+strlen("alpha|βeta")&&record[0]=='Z'&&record[1]=='T'&&record[2]=='C'&&record[3]==2);
    assert(Config::get32(record.data()+4)==1&&get16(record.data()+8)==strlen("alpha|βeta")&&!record[10]&&!record[11]);
    assert(!memcmp(record.data()+12,"alpha|βeta",strlen("alpha|βeta")));
    assert(Config::get32(record.data()+record.size()-4)==crc32(record.data(),record.size()-4));
    CannedMessageJournal reboot;assert(!reboot.load(b)&&reboot.state()==CannedMessageJournal::State::Loaded&&reboot.config().same(first));
    const auto writeCount=b.writes.size();assert(!reboot.save(b,first)&&b.writes.size()==writeCount&&reboot.generation()==1);
    auto second=settings("last|entry");assert(!reboot.save(b,second)&&reboot.generation()==2&&reboot.activeSlot()==1);
    CannedMessageJournal latest;assert(!latest.load(b)&&latest.config().same(second)&&latest.generation()==2);
    pass("compact V2 record, exact UTF-8 bytes, verified alternating save/reload, unchanged write suppression");
  }
  {
    Backend b;const auto original=settings("legacy|Maōri");
    b.rows["mtcanned/state0"]=legacyRecord(original,41);const auto legacy=b.rows.at("mtcanned/state0");
    CannedMessageJournal loaded;assert(!loaded.load(b)&&loaded.state()==CannedMessageJournal::State::Loaded&&
      loaded.generation()==41&&loaded.activeSlot()==0&&loaded.config().same(original)&&b.writes.empty());
    assert(!loaded.save(b,original)&&b.writes.empty()&&b.rows.at("mtcanned/state0")==legacy);
    const auto changed=settings("changed");assert(!loaded.save(b,changed)&&loaded.generation()==42&&loaded.activeSlot()==1);
    assert(b.rows.at("mtcanned/state0")==legacy&&b.rows.at("mtcanned/state1").size()==16+changed.length());
    CannedMessageJournal reboot;assert(!reboot.load(b)&&reboot.config().same(changed)&&reboot.generation()==42);
    pass("legacy V1 loads byte-exactly, no-op leaves it untouched, next changed save uses compact V2");
  }
  {
    Backend b;CannedMessageJournal j;assert(!j.load(b));
    CannedMessageSettings ignored;const auto defaults=ignored;
    assert(ignored.assign("",0)==CannedMessageSettings::AssignResult::IgnoredEmpty&&ignored.same(defaults));
    auto emptyV2=Bytes(16);emptyV2[0]='Z';emptyV2[1]='T';emptyV2[2]='C';emptyV2[3]=2;seal(emptyV2);
    expectInvalidOnly(emptyV2);
    std::string full(200,'f');assert(!j.save(b,settings(full.c_str()))&&b.rows.at("mtcanned/state0").size()==216);
    CannedMessageJournal fullRead;assert(!fullRead.load(b)&&fullRead.config().length()==200);
    pass("empty setter remains ignored, zero-length V2 is invalid, and full 200-byte value uses 216 bytes");
  }
  {
    auto base=legacyRecord(settings("legacy-cut"),8);
    for(size_t cut=0;cut<base.size();++cut){
      auto b=Backend{};b.rows["mtcanned/state0"]=base;b.rows["mtcanned/state0"].resize(cut);
      expectInvalidOnly(b.rows.at("mtcanned/state0"));
    }
    auto b=Backend{};b.rows["mtcanned/state0"]=base;CannedMessageJournal j;
    assert(!j.load(b)&&j.config().same(settings("legacy-cut")));
    pass("legacy V1 accepts exactly224 bytes and rejects every truncated length");
  }
  {
    const auto valid=legacyRecord(settings("same-generation"),5);
    auto other=settings("different-list");Backend b;b.rows["mtcanned/state0"]=valid;
    b.rows["mtcanned/state1"]=legacyRecord(other,5);CannedMessageJournal sameGeneration;
    assert(sameGeneration.load(b)==-EINVAL&&sameGeneration.state()==CannedMessageJournal::State::Invalid);
    pass("legacy same-generation divergent records remain ambiguous");
  }
  {
    Backend b;
    b.rows["mtbase/config"]={1,2,3,4};b.rows["mtmod/state1"]={5,6,7};
    b.rows["mtextra/state0"]={8,9};b.rows["mtidentity/state0"]={10,11};const auto protectedRows=b.rows;
    b.reads.clear();b.writes.clear();CannedMessageJournal j;assert(!j.load(b)&&!j.save(b,settings("saved|list")));
    for(const auto &key:b.reads)assert(key=="mtcanned/state0"||key=="mtcanned/state1");
    for(const auto &key:b.writes)assert(key=="mtcanned/state0"||key=="mtcanned/state1");
    for(const auto &row:protectedRows)assert(b.rows.at(row.first)==row.second);
    CannedMessageJournal reboot;assert(!reboot.load(b)&&reboot.config().same(settings("saved|list")));
    pass("only mtcanned slot keys are touched; existing core, module, extended, and identity keys remain unchanged");
  }
  {
    auto b=two();const auto old=settings("one|two");
    const size_t compactSize=b.rows.at("mtcanned/state1").size();
    for(size_t cut=0;cut<compactSize;++cut){
      auto truncated=b;truncated.rows["mtcanned/state1"].resize(cut);const auto before=truncated.rows;
      CannedMessageJournal j;assert(!j.load(truncated)&&j.state()==CannedMessageJournal::State::Recovered&&
        j.config().same(old)&&j.generation()==1&&j.activeSlot()==0&&truncated.rows==before&&truncated.writes.size()==2);
      auto only=truncated.rows.at("mtcanned/state0");
      only.resize(cut<only.size()?cut:only.size()-1);expectInvalidOnly(only);
    }
    auto tooLong=b.rows.at("mtcanned/state0");tooLong.push_back(0);expectInvalidOnly(tooLong);
    pass("all compact truncation boundaries recover only an intact prior slot; lone truncated/oversized records fail closed");
  }
  {
    auto b=two();const auto old=settings("one|two");
    for(size_t i=0;i<b.rows.at("mtcanned/state1").size();++i){
      auto damaged=b;damaged.rows["mtcanned/state1"][i]^=1;const auto before=damaged.rows;
      CannedMessageJournal j;assert(!j.load(damaged)&&j.state()==CannedMessageJournal::State::Recovered&&j.config().same(old)&&damaged.rows==before);
    }
    pass("single-byte corruption at every record offset falls back read-only to valid old state");
  }
  {
    auto b=two();auto &other=b.rows["mtcanned/state1"];
    Config::put32(other.data()+4,1);other[12]^=1;seal(other);
    CannedMessageJournal conflict;assert(conflict.load(b)==-EINVAL&&!conflict.ready()&&conflict.state()==CannedMessageJournal::State::Invalid);
    auto half=two();Config::put32(half.rows["mtcanned/state1"].data()+4,0x80000001u);seal(half.rows["mtcanned/state1"]);
    CannedMessageJournal ambiguous;assert(ambiguous.load(half)==-EINVAL&&!ambiguous.ready());
    pass("conflicting same-generation and half-range slot ordering refuses to choose a message list");
  }
  {
    Backend b;CannedMessageJournal j;assert(!j.load(b)&&!j.save(b,settings("utf8")));
    const Bytes good=b.rows.at("mtcanned/state0");
    for(const auto mutate:{0,1}){
      auto bad=good;
      if(mutate==0){bad[10]=1;}
      if(mutate==1){bad[8]=uint8_t(get16(bad.data()+8)+1);}
      seal(bad);expectInvalidOnly(bad);
    }
    auto trailing=good;trailing.push_back(0);expectInvalidOnly(trailing);
    auto shortCrc=good;shortCrc.pop_back();expectInvalidOnly(shortCrc);
    pass("V2 rejects reserved bits, noncanonical lengths, embedded NUL, trailing bytes, and truncated CRC");
  }
  {
    const char opaque[]={char(0xff),'x'};CannedMessageSettings value;
    assert(value.assign(opaque,sizeof(opaque))==CannedMessageSettings::AssignResult::Changed);
    Backend b;CannedMessageJournal j;assert(!j.load(b)&&!j.save(b,value));
    CannedMessageJournal reload;assert(!reload.load(b)&&reload.config().same(value));
    pass("legacy opaque non-NUL bytes remain accepted byte-exactly despite non-UTF-8 sequences");
  }
  {
    Backend b;CannedMessageJournal j;assert(!j.load(b)&&!j.save(b,settings("before-wrap")));
    Config::put32(b.rows["mtcanned/state0"].data()+4,UINT32_MAX);seal(b.rows["mtcanned/state0"]);
    CannedMessageJournal reboot;assert(!reboot.load(b)&&reboot.generation()==UINT32_MAX&&reboot.activeSlot()==0);
    assert(!reboot.save(b,settings("after-wrap"))&&reboot.generation()==0&&reboot.activeSlot()==1);
    CannedMessageJournal latest;assert(!latest.load(b)&&latest.generation()==0&&latest.config().same(settings("after-wrap")));
    pass("generation wraps UINT32_MAX to zero and slot selection survives reload");
  }
  {
    for(auto fault:{Backend::Fault::BeforeWrite,Backend::Fault::CommittedError,Backend::Fault::TornError,
                    Backend::Fault::TornSuccess,Backend::Fault::ReadbackError,Backend::Fault::ReadbackShort,
                    Backend::Fault::ReadbackLong,Backend::Fault::ReadbackDifferent}){
      Backend b;CannedMessageJournal j;assert(!j.load(b)&&!j.save(b,settings("old")));
      const auto old=j.config();const auto generation=j.generation();const auto slot=j.activeSlot();b.fault=fault;
      assert(j.save(b,settings("new"))<0&&j.state()==CannedMessageJournal::State::IoError&&!j.ready()&&
        j.config().same(old)&&j.generation()==generation&&j.activeSlot()==slot);
      const auto writeCount=b.writes.size();const auto rows=b.rows;
      assert(j.save(b,old)==-EACCES&&j.save(b,settings("again"))==-EACCES&&b.writes.size()==writeCount&&b.rows==rows);
      b.fault=Backend::Fault::None;b.afterWrite=false;CannedMessageJournal reboot;assert(!reboot.load(b));
      const bool committed=fault==Backend::Fault::CommittedError||fault==Backend::Fault::ReadbackError||
        fault==Backend::Fault::ReadbackShort||fault==Backend::Fault::ReadbackLong||fault==Backend::Fault::ReadbackDifferent;
      assert(reboot.config().same(settings(committed?"new":"old")));
    }
    pass("uncertain writes/readbacks preserve old RAM and poison retries until reload; reload reflects durable outcome");
  }
  {
    Backend b;b.fault=Backend::Fault::ReadError;CannedMessageJournal j;
    assert(j.load(b)==-EIO&&!j.ready()&&j.state()==CannedMessageJournal::State::IoError&&b.writes.empty());
    assert(j.save(b,settings("blocked"))==-EACCES&&b.writes.empty());
    b.fault=Backend::Fault::None;assert(!j.load(b)&&j.ready()&&j.config().same(CannedMessageSettings{}));
    pass("backend read errors fail closed without seeding defaults or writing; explicit reload recovers absent state");
  }
  std::printf("LAYOUT settings=%zu journal=%zu live_record=%zu slot_record=%zu nvs_two_slots=%zu\n",
    sizeof(CannedMessageSettings),sizeof(CannedMessageJournal),CannedMessageSettings::Capacity,
    CannedMessageJournal::RecordSize,2*CannedMessageJournal::RecordSize);
  std::printf("CANNED_MESSAGE_JOURNAL_GROUPS %u\n",groups);
}
