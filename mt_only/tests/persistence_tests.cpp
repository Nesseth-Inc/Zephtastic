// SPDX-License-Identifier: GPL-3.0-only
#include "SettingsJournal.h"
#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>
using Bytes=std::vector<uint8_t>;
struct Memory final:mt::SettingsBackend {
  std::unordered_map<std::string,Bytes> files;
  int writes=0,writeError=0,readError=0,truncate=-1;bool failAfterWrite=false,corruptAfterWrite=false,errorAfterCommit=false;
  int read(const char *key,uint8_t *out,size_t capacity)override{
    if(readError)return readError;
    auto i=files.find(key);if(i==files.end())return -ENOENT;
    if(i->second.size()>capacity)return -E2BIG;
    if(!i->second.empty())memcpy(out,i->second.data(),i->second.size());
    return int(i->second.size());
  }
  int write(const char *key,const uint8_t *data,size_t length)override{
    ++writes;if(writeError)return writeError;
    if(truncate>=0&&size_t(truncate)<length){files[key]=Bytes(data,data+truncate);return -EIO;}
    files[key]=Bytes(data,data+length);if(corruptAfterWrite)files[key][30]^=1;
    if(errorAfterCommit)return -EIO;
    if(failAfterWrite)readError=-EIO;
    return 0;
  }
};
static Bytes legacy(const mt::Config &c){
  Bytes b(c.bytes,c.bytes+78);b[2]=1;const uint16_t sum=mt::Config::checksum(b.data(),b.size());
  b.push_back(uint8_t(sum));b.push_back(uint8_t(sum>>8));return b;
}
static Bytes previous(const mt::Config &c,uint8_t version=3){
  assert(version==2||version==3);Bytes b(mt::Config::PreviousSize,0);memcpy(b.data(),c.bytes,84);b[2]=version;
  if(version==2)b[83]=0;
  const uint16_t sum=mt::Config::checksum(b.data(),94);b[94]=uint8_t(sum);b[95]=uint8_t(sum>>8);return b;
}
static Bytes currentPrevious(const mt::Config &c,uint8_t version){
  assert(version>=4&&version<=10);Bytes b(c.bytes,c.bytes+mt::Config::FormerSize);b[2]=version;memset(b.data()+107,0,3);
  if(version==4)memset(b.data()+97,0,13);
  else if(version==5)memset(b.data()+101,0,9);
  else {if(version<7)b[102]=0;if(version<9)b[103]=0;if(version<10)memset(b.data()+104,0,6);}
  const uint16_t sum=mt::Config::checksum(b.data(),b.size()-2);b[b.size()-2]=uint8_t(sum);b[b.size()-1]=uint8_t(sum>>8);return b;
}
static Bytes previousJournal(const mt::Config &c,uint32_t generation,uint8_t version=3){
  Bytes b(mt::SettingsJournal::PreviousRecordSize,0);b[0]='Z';b[1]='T';b[2]='J';b[3]=1;
  for(unsigned i=0;i<4;++i)b[4+i]=uint8_t(generation>>(8*i));
  b[8]=mt::Config::PreviousSize;
  const auto config=previous(c,version);memcpy(b.data()+12,config.data(),config.size());
  uint32_t crc=UINT32_MAX;for(size_t i=0;i<b.size()-4;++i){crc^=b[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
  crc=~crc;for(unsigned i=0;i<4;++i)b[b.size()-4+i]=uint8_t(crc>>(8*i));return b;
}
static Bytes formerJournal(const mt::Config &c,uint32_t generation){
  const auto config=currentPrevious(c,10);Bytes b(mt::SettingsJournal::FormerRecordSize,0);b[0]='Z';b[1]='T';b[2]='J';b[3]=2;
  for(unsigned i=0;i<4;++i)b[4+i]=uint8_t(generation>>(8*i));
  b[8]=config.size();memcpy(b.data()+12,config.data(),config.size());
  uint32_t crc=UINT32_MAX;for(size_t i=0;i<b.size()-4;++i){crc^=b[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));}
  crc=~crc;for(unsigned i=0;i<4;++i)b[b.size()-4+i]=uint8_t(crc>>(8*i));return b;
}
static mt::Config named(const char *name,bool publicChannel=true){
  mt::Config c;c.bytes[4]=1;auto n=c.node();assert(n.setName(name,false));assert(n.setName("TST",true));
  if(!publicChannel)n.setKey(0,nullptr,0);
  c.node(n);return c;
}
static void generation(Bytes &b,uint32_t value){
  for(unsigned i=0;i<4;++i)b[4+i]=uint8_t(value>>(8*i));
  uint32_t c=UINT32_MAX;for(size_t i=0;i<b.size()-4;++i){c^=b[i];for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}
  c=~c;for(unsigned i=0;i<4;++i)b[b.size()-4+i]=uint8_t(c>>(8*i));
}
static void expectCurrentRecord(const Bytes &record,const mt::Config &config,
                                uint32_t expectedGeneration,uint32_t expectedChannelGeneration){
  assert(record.size()==656 && !memcmp(record.data(),"ZTJ\x07",4));
  assert(mt::Config::get32(record.data()+4)==expectedGeneration);
  assert(mt::Config::get32(record.data()+8)==expectedChannelGeneration);
  assert(!memcmp(record.data()+12,config.bytes,640));
}
static unsigned groups=0;
#define PASS(s) do{++groups;puts(s);}while(0)
int main(){
  {mt::Config c;assert(c.valid()&&c.bytes[2]==mt::Config::Version&&sizeof(c)==640&&c.role()==1&&c.rebroadcastMode()==0&&!c.primaryName()[0]&&c.node().isPublic()&&!c.provisioned()&&c.displaySeconds()==15&&!c.displayFlip()&&!c.displayCarouselSeconds()&&!c.txIntentSpecified()&&!c.fixedPosition()&&!c.channelMuted());
   for(unsigned i=0;i<sizeof(c.bytes);++i)for(unsigned bit=0;bit<8;++bit){auto bad=c;bad.bytes[i]^=1u<<bit;assert(!bad.valid());}
   for(uint32_t seconds:{0u,1u,60u,86400u,UINT32_MAX}){assert(c.setDisplay(seconds,true)&&c.valid()&&c.displaySeconds()==seconds&&c.displayFlip());}
   const auto before=c;assert(!c.setDisplay(86401,false)&&c.same(before));assert(!c.setDisplay(UINT32_MAX-1,false));
   assert(c.setDisplay(15,false,86400)&&c.displayCarouselSeconds()==86400);
   const auto bounded=c;assert(!c.setDisplay(15,false,86401)&&c.same(bounded));
   for(bool enabled:{false,true}){auto intent=c;intent.setTxEnabled(enabled);assert(intent.valid()&&intent.txIntentSpecified()&&intent.txEnabled()==enabled);}
   {auto bad=c;bad.bytes[83]=3;bad.seal();assert(!bad.valid());}
   assert(c.setFixedPosition(-900000000,1800000000,true,INT32_MIN)&&c.valid()&&c.fixedPosition()&&c.fixedAltitude());
   assert(c.latitudeI()==-900000000&&c.longitudeI()==1800000000&&c.altitude()==INT32_MIN);
   c.clearFixedPosition();assert(c.valid()&&!c.fixedPosition()&&!c.fixedAltitude());
   assert(!c.setFixedPosition(-900000001,0,false,0)&&!c.fixedPosition());
   c.setChannelMuted(true);assert(c.valid()&&c.channelMuted());c.setChannelMuted(false);assert(c.valid()&&!c.channelMuted());
   {auto bad=c;bad.bytes[101]=2;bad.seal();assert(!bad.valid());}
   for(unsigned precision:{0u,1u,15u,32u}){auto supported=c;supported.bytes[122]=uint8_t(precision);supported.seal();mt::Config out;
    assert(supported.valid()&&mt::Config::decode(supported.bytes,supported.Size,out)&&out.same(supported)&&out.primaryPositionPrecision()==precision&&!out.positionPrecision(0));}
   for(unsigned precision:{33u,255u}){auto bad=c;bad.bytes[122]=uint8_t(precision);bad.seal();assert(!bad.valid());}
   for(unsigned i:{102u,106u,125u}){auto bad=c;bad.bytes[i]=1;bad.seal();assert(!bad.valid());}
   // MQTT consent fields were assigned in v17; malformed booleans still fail.
   for(unsigned i:{123u,124u}){auto bad=c;bad.bytes[i]=2;bad.seal();assert(!bad.valid());}
   for(auto field:std::vector<std::pair<unsigned,uint8_t>>{{107,3},{108,1},{109,1}}){auto bad=c;bad.bytes[field.first]=field.second;bad.seal();assert(!bad.valid());}
   {auto bad=c;bad.bytes[104]=17;bad.seal();assert(!bad.valid());}
   {auto bad=c;bad.bytes[105]=105;bad.seal();assert(!bad.valid());}
   {auto bad=c;bad.bytes[84]=mt::Config::AltitudeValid;bad.seal();assert(!bad.valid());}
   PASS("Current Config: exact640 size, corruption, display carousel, TX intent, fixed position, mute, role/name, precision and field validation");}
  {for(bool enabled:{false,true}){auto old=named("Saved owner",enabled);auto b=legacy(old);mt::Config restored;
    assert(mt::Config::decode(b.data(),b.size(),restored));assert(restored.node().isPublic()==enabled&&restored.bytes[4]==1);
    assert(!memcmp(old.bytes+6,restored.bytes+6,72)&&restored.displaySeconds()==15&&!restored.displayFlip());
    for(size_t i=0;i<b.size();++i){auto corrupt=b;corrupt[i]^=1;auto unchanged=restored;assert(!mt::Config::decode(corrupt.data(),corrupt.size(),restored)&&restored.same(unchanged));}
   }PASS("v1 migration preserves names, Public/disabled key and US; output unchanged on failed decode");}
  {const auto base=named("Previous config");for(uint8_t version:{uint8_t(2),uint8_t(3)}){
    auto bytes=previous(base,version);mt::Config restored;assert(mt::Config::decode(bytes.data(),bytes.size(),restored));
    assert(restored.bytes[2]==mt::Config::Version&&!restored.fixedPosition()&&!restored.displayCarouselSeconds()&&!restored.channelMuted()&&!memcmp(restored.bytes+6,base.bytes+6,77));
    assert(restored.txIntent()==(version==2?mt::Config::TxIntent::Unspecified:base.txIntent()));
    for(size_t i=0;i<bytes.size();++i){auto corrupt=bytes;corrupt[i]^=1;auto unchanged=restored;
      assert(!mt::Config::decode(corrupt.data(),corrupt.size(),restored)&&restored.same(unchanged));}
   }PASS("v2/v3 Config migration preserves all prior fields and adds no synthetic position");}
  {auto base=named("V4/V5 config");assert(base.setDisplay(123,true,45));base.setTxEnabled(true);
   for(uint8_t version:{uint8_t(4),uint8_t(5)}){
    auto bytes=currentPrevious(base,version);mt::Config restored;assert(mt::Config::decode(bytes.data(),bytes.size(),restored));
    assert(restored.bytes[2]==mt::Config::Version&&!restored.channelMuted()&&restored.txEnabled()&&restored.displaySeconds()==123&&restored.displayFlip());
    assert(restored.displayCarouselSeconds()==(version==4?0:45));
    auto forbidden=bytes;forbidden[101]=1;const auto sum=mt::Config::checksum(forbidden.data(),forbidden.size()-2);
    forbidden[forbidden.size()-2]=uint8_t(sum);forbidden.back()=uint8_t(sum>>8);auto unchanged=restored;
    assert(!mt::Config::decode(forbidden.data(),forbidden.size(),restored)&&restored.same(unchanged));
   }PASS("v4/v5 Config migration defaults channel mute false and never reinterprets formerly reserved byte 101");}
  {Memory b;const auto old=named("Old journal");const auto original=previousJournal(old,9,3);
   b.files["mtbase/state0"]=original;
   mt::SettingsJournal j;assert(!j.load(b)&&j.config().same(old)&&j.generation()==9&&j.activeSlot()==0&&b.writes==0);
   auto next=old;assert(next.setFixedPosition(123,456,true,0));assert(!j.save(b,next));
   assert(b.files.at("mtbase/state0")==original);
   expectCurrentRecord(b.files.at("mtbase/state1"),next,10,0);
   mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(next)&&reboot.generation()==10&&reboot.channelGeneration()==0);
   PASS("original version-1 journal survives migration beside the version-7 fixed-position record");}
  {Memory b;const auto old=named("Former journal");const auto original=formerJournal(old,17);
   b.files["mtbase/state0"]=original;
   assert(original.size()==128&&original[3]==2&&original[8]==112);mt::SettingsJournal j;assert(!j.load(b)&&j.config().same(old)&&j.generation()==17&&!b.writes);
   auto next=old;assert(next.setDeviceRole(0,0)&&next.setPrimaryName("M93"));assert(!j.save(b,next));
   assert(b.files.at("mtbase/state0")==original);
   expectCurrentRecord(b.files.at("mtbase/state1"),next,18,1);
   mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(next)&&reboot.generation()==18&&reboot.channelGeneration()==1);
   PASS("original version-2 journal survives migration beside the version-7 channel rename");}
  {Memory b;const auto old=named("V6 journal");auto historical=old;historical.bytes[2]=16;historical.seal();
   Bytes original(656,0);
   memcpy(original.data(),"ZTJ\x06",4);original[8]=0x80;original[9]=0x02;  // v6 payload length, 640.
   memcpy(original.data()+12,historical.bytes,640);generation(original,41);
   b.files["mtbase/state0"]=original;
   mt::SettingsJournal j;assert(!j.load(b)&&j.config().same(old)&&j.generation()==41&&j.channelGeneration()==0&&!b.writes);
   auto next=old;assert(next.setDisplay(120,true)&&!j.save(b,next));
   assert(b.files.at("mtbase/state0")==original);
   expectCurrentRecord(b.files.at("mtbase/state1"),next,42,0);
   mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(next)&&reboot.generation()==42&&reboot.channelGeneration()==0);
   PASS("version-6 payload length is accepted on read and version-7 writes the channel generation");}
  {Memory b;mt::SettingsJournal j;assert(!j.load(b)&&j.ready()&&j.state()==mt::SettingsJournal::State::Fresh&&b.writes==0);
   assert(j.config().valid()&&!j.config().provisioned()&&j.activeSlot()==-1);
   const auto c=named("First config");assert(!j.save(b,c)&&j.generation()==1&&j.activeSlot()==0&&b.writes==1);
   assert(!j.save(b,c)&&b.writes==1);mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(c)&&b.writes==1);
   auto next=named("Second config");assert(next.setDisplay(60,true));assert(!j.save(b,next)&&j.activeSlot()==1&&j.generation()==2);
   assert(!reboot.load(b)&&reboot.config().same(next));PASS("fresh/save/reboot selects latest, alternates slots and elides unchanged writes");}
  {Memory b;const auto old=named("Legacy owner",false);b.files["mtbase/config"]=legacy(old);const auto legacyCopy=b.files["mtbase/config"];
   mt::SettingsJournal j;assert(!j.load(b)&&j.state()==mt::SettingsJournal::State::Legacy&&b.writes==0);
   assert(j.config().same(old));assert(!j.save(b,j.config())&&j.activeSlot()==0&&b.writes==1);
   assert(b.files["mtbase/config"]==legacyCopy);mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(old));
   PASS("legacy migration is read-only until explicit save and preserves rollback bytes");}
  {const auto old=named("Old valid"),next=named("New desired");
   for(unsigned cut=0;cut<mt::SettingsJournal::RecordSize;++cut){
    Memory b;mt::SettingsJournal j;assert(!j.load(b)&&!j.save(b,old));b.truncate=int(cut);assert(j.save(b,next)==-EIO&&j.config().same(old));
    b.truncate=-1;mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(old)&&reboot.state()==mt::SettingsJournal::State::Recovered);
    assert(!reboot.save(b,next));mt::SettingsJournal third;assert(!third.load(b)&&third.config().same(next));
   }PASS("all current partial-write cut points recover prior slot, then allow a successful retry");}
  {const auto old=named("Preserved v1"),next=named("New display");
   for(unsigned cut=0;cut<mt::SettingsJournal::RecordSize;++cut){Memory b;b.files["mtbase/config"]=legacy(old);
    mt::SettingsJournal j;assert(!j.load(b));b.truncate=int(cut);assert(j.save(b,next)==-EIO);
    b.truncate=-1;mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(old));
   }PASS("all migration write cut points recover unchanged legacy config");}
  {Memory base;mt::SettingsJournal j;const auto old=named("Older valid"),latest=named("Latest valid");assert(!j.load(base)&&!j.save(base,old)&&!j.save(base,latest));
   for(size_t i=0;i<mt::SettingsJournal::RecordSize;++i)for(unsigned bit=0;bit<8;++bit){auto b=base;b.files["mtbase/state1"][i]^=1u<<bit;mt::SettingsJournal reboot;
    assert(!reboot.load(b)&&reboot.config().same(old)&&reboot.state()==mt::SettingsJournal::State::Recovered);}
   PASS("every latest-record single-bit corruption recovers previous config");}
  {Memory b;mt::SettingsJournal j;auto old=named("Before error"),next=named("After error");assert(!j.load(b)&&!j.save(b,old));
   b.writeError=-ENOSPC;assert(j.save(b,next)==-ENOSPC&&!j.ready()&&j.config().same(old));b.writeError=0;assert(!j.load(b));
   b.corruptAfterWrite=true;assert(j.save(b,next)==-EIO&&!j.ready()&&j.config().same(old));assert(j.save(b,next)==-EACCES);
   b.corruptAfterWrite=false;assert(!j.load(b)&&j.config().same(old));assert(!j.save(b,next));
   PASS("write/no-space and corrupt readback never publish unverified config");}
  {Memory b;mt::SettingsJournal j;auto old=named("RAM old"),next=named("Committed with error");assert(!j.load(b)&&!j.save(b,old));
   b.errorAfterCommit=true;assert(j.save(b,next)==-EIO&&!j.ready()&&j.config().same(old));
   b.errorAfterCommit=false;assert(j.save(b,old)==-EACCES);assert(!j.load(b)&&j.config().same(next));
   assert(!j.save(b,old));mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(old));
   PASS("full commit with write error blocks stale no-op acknowledgement until reload");}
  {Memory b;mt::SettingsJournal j;auto old=named("Before reset"),next=named("Committed at reset");assert(!j.load(b)&&!j.save(b,old));
   b.failAfterWrite=true;assert(j.save(b,next)==-EIO&&!j.ready()&&j.config().same(old));
   b.failAfterWrite=false;b.readError=0;mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(next));
   PASS("reset after committed write may load new valid config; uncertain readback blocks further writes");}
  {Memory b;b.files["mtbase/state0"]=Bytes(mt::SettingsJournal::RecordSize+1,1);mt::SettingsJournal j;assert(j.load(b)==-EINVAL&&!j.ready());
   assert(j.save(b,mt::Config{})==-EACCES&&b.writes==0);b.files["mtbase/config"]=Bytes(80,0);assert(j.load(b)==-EINVAL);
   b.readError=-EIO;assert(j.load(b)==-EIO&&j.state()==mt::SettingsJournal::State::IoError);PASS("no valid fallback fails closed, unknown record sizes and backend I/O are not factory reset");}
  {Memory b;mt::SettingsJournal j;auto a=named("Generation A"),z=named("Generation B");assert(!j.load(b)&&!j.save(b,a)&&!j.save(b,z));
   generation(b.files["mtbase/state0"],UINT32_MAX);generation(b.files["mtbase/state1"],0);
   assert(!j.load(b)&&j.config().same(z)&&j.generation()==0);assert(!j.save(b,a)&&j.generation()==1);
   generation(b.files["mtbase/state1"],1);assert(j.load(b)==-EINVAL&&!j.ready());
   generation(b.files["mtbase/state1"],0x80000001u);assert(j.load(b)==-EINVAL&&!j.ready());
   PASS("generation wraps, but same-generation disagreement and half-range ambiguity fail closed");}
  printf("PASS %u persistence groups; no hardware or RF\n",groups);
  printf("PERSISTENCE_METADATA record_size=%zu partial_write_cut_points_per_path=%zu corruption_single_bits=%zu\n",
    mt::SettingsJournal::RecordSize,mt::SettingsJournal::RecordSize,mt::SettingsJournal::RecordSize*8);
}
