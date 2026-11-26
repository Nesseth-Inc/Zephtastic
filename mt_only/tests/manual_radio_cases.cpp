#include "Config.h"
#include "SettingsJournal.h"
#include <cassert>
#include <fstream>
#include <map>
#include <vector>
#include <string>
struct Backend:mt::SettingsBackend{std::map<std::string,std::vector<uint8_t>> values;bool fail=false;int read(const char*k,uint8_t*out,size_t cap)override{auto it=values.find(k);if(it==values.end())return -ENOENT;if(it->second.size()>cap)return -E2BIG;memcpy(out,it->second.data(),it->second.size());return int(it->second.size());}int write(const char*k,const uint8_t*p,size_t n)override{values[k]={p,p+n};return fail?-EIO:0;}};
static std::vector<uint8_t> read(const char*n){std::ifstream f(n,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
#include <iostream>
#include <limits>
int main(){
 mt::Config c;assert(c.valid()&&sizeof(c)==640&&c.bytes[2]==mt::Config::Version&&c.channelNumber()==20);assert(c.effectivePower(true)==-9&&c.effectivePower(false)==2);std::cout<<"PASS current Config defaults retain operating power and channel\n";
 auto old=read("v13-config.bin");mt::Config migrated;assert(old.size()==576&&mt::Config::decode(old.data(),old.size(),migrated));assert(migrated.valid()&&migrated.usePreset()&&migrated.channelNumber()==20&&!migrated.powerSpecified());assert(!memcmp(migrated.bytes+3,old.data()+3,571));std::cout<<"PASS actual Config13 bytes migrate without channel/key changes\n";
 auto request=c.radioRequest();request.usePreset=false;request.channel=0;assert(c.setRadio(request)&&!strcmp(c.effectivePrimaryName(),"Custom"));mt::LoRaPresets::Profile p{};mt::USChannelSelection s{};assert(c.resolveRadio(p,s)&&s.frequencyHz==906125000&&p.bandwidthHz==250000&&p.spreadingFactor==11&&p.codingDenominator==5);std::cout<<"PASS manual defaults use Custom name and stock auto channel\n";
 const unsigned bw[]={8,10,16,21,31,42,62,125,250,500};for(auto value:bw){request.bandwidth=value;request.spreadingFactor=5;request.codingRate=8;assert(c.setRadio(request)&&c.resolveRadio(p,s)&&p.spreadingFactor==5&&p.codingDenominator==8);}std::cout<<"PASS all ten manual widths persist and resolve\n";
 request.regionCode=1;request.bandwidth=250;request.spreadingFactor=12;request.codingRate=5;request.powerSpecified=true;request.power=0;request.overrideMHz=915.5f;request.offsetMHz=.25f;assert(c.setRadio(request)&&c.resolveRadio(p,s)&&s.frequencyHz==915750000&&c.effectivePower(true)==22&&c.radioRequest().power==30);std::cout<<"PASS override offset and regional/chip power selection\n";
 auto before=c;request.codingRate=4;assert(!c.setRadio(request)&&c.same(before));request.codingRate=5;request.offsetMHz=20;assert(!c.setRadio(request)&&c.same(before));request.offsetMHz=std::numeric_limits<float>::quiet_NaN();assert(!c.setRadio(request)&&c.same(before));std::cout<<"PASS unsupported CR4 and invalid frequencies reject atomically\n";
 auto aead=c;auto keyed=aead.node();mt::PrimaryChannel::Selection key{};
 assert(mt::PrimaryChannel::normalize(bench::MtNodeRecord::PublicKey,sizeof(bench::MtNodeRecord::PublicKey),key,aead.effectivePrimaryName()));
 keyed.setKey(key.hash,key.bytes,key.length);aead.node(keyed);mt::PrimaryChannel::clear(key);
 assert(aead.channelConfigured()&&aead.setPrimaryAead(true));mt::Config aeadRoundtrip;
 assert(mt::Config::decode(aead.bytes,sizeof(aead.bytes),aeadRoundtrip)&&aeadRoundtrip.same(aead)&&aeadRoundtrip.primaryAead());
 mt::PrimaryChannel::Selection effective{};assert(aeadRoundtrip.channelSelection(0,effective)&&effective.aead&&effective.hash==uint8_t(aeadRoundtrip.node().bytes[4]^0xAEu));mt::PrimaryChannel::clear(effective);
 mt::Config roundtrip;assert(mt::Config::decode(c.bytes,sizeof(c.bytes),roundtrip)&&roundtrip.same(c));
 auto invalidMode=c;invalidMode.bytes[604]=2;invalidMode.seal();assert(!mt::Config::decode(invalidMode.bytes,sizeof(invalidMode.bytes),roundtrip)&&roundtrip.same(c));
 auto reserved=c;reserved.bytes[605]=1;reserved.seal();assert(!mt::Config::decode(reserved.bytes,sizeof(reserved.bytes),roundtrip)&&roundtrip.same(c));
 old[574]^=1;assert(!mt::Config::decode(old.data(),old.size(),roundtrip)&&roundtrip.same(c));std::cout<<"PASS AEAD and CTR roundtrips; invalid mode, reserved byte and old CRC refuse atomically\n";
 Backend backend;backend.values["mtbase/state0"]=read("v4-journal.bin");mt::SettingsJournal journal;assert(!journal.load(backend)&&journal.config().valid()&&journal.generation()==1);assert(!journal.save(backend,c)&&journal.generation()==2);auto &bytes=backend.values["mtbase/state1"];assert(bytes.size()==656&&bytes[3]==6);mt::SettingsJournal reboot;assert(!reboot.load(backend)&&reboot.config().same(c));std::cout<<"PASS actual journal4 migrates to journal6 and reboots\n";
 backend.fail=true;auto change=c;change.setTxEnabled(true);assert(journal.save(backend,change)==-EIO&&journal.state()==mt::SettingsJournal::State::IoError&&journal.config().same(c));assert(journal.save(backend,c)==-EACCES);std::cout<<"PASS uncertain settings commit retains RAM and stops later writes\n";
}
