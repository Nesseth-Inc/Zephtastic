// Synthetic native tests. No real keys, storage, radio, network or clock.
#include "Config.h"
#include "SettingsJournal.h"
#include "PositionQuery.h"
#include "stock/position/PositionChannels.h"
#include "stock_precision_reference.h"
#include "ui/Model.h"
#include "ui/RadioPresentation.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
static bool sameEncodedPosition(const meshtastic_Position &a,const meshtastic_Position &b){
 uint8_t x[233]{},y[233]{};auto sx=pb_ostream_from_buffer(x,sizeof(x)),sy=pb_ostream_from_buffer(y,sizeof(y));
 return pb_encode(&sx,meshtastic_Position_fields,&a)&&pb_encode(&sy,meshtastic_Position_fields,&b)&&sx.bytes_written==sy.bytes_written&&!memcmp(x,y,sx.bytes_written);
}
static std::vector<uint8_t> read(const char *name){std::ifstream f(name,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
struct Backend:mt::SettingsBackend {
 std::map<std::string,std::vector<uint8_t>> records;bool fail=false;
 int read(const char *k,uint8_t *out,size_t cap)override{auto i=records.find(k);if(i==records.end())return -ENOENT;if(i->second.size()>cap)return -E2BIG;memcpy(out,i->second.data(),i->second.size());return int(i->second.size());}
 int write(const char *k,const uint8_t *data,size_t n)override{records[k]={data,data+n};return fail?-EIO:0;}
};
static mt::Config initial(){mt::Config c;c.bytes[4]=1;c.seal();assert(c.valid()&&c.setFixedPosition(123456789,-123456789,true,-35));return c;}
static mt::ChannelRecord channel(unsigned role,const char *name,const uint8_t *key,size_t length,unsigned precision=0,bool mute=false){mt::ChannelRecord c;assert(c.set(role,name,key,length,precision,mute));return c;}
static bool sameKey(const mt::PrimaryChannel::Selection &a,const mt::PrimaryChannel::Selection &b){return a.length==b.length&&a.hash==b.hash&&!memcmp(a.bytes,b.bytes,a.length);}
static meshtastic_Position response(const mt::Config &c,unsigned channel){
 uint8_t raw[128]{};size_t n=0;assert(mt::PositionQuery::fixedResponse(c,0x761,raw,sizeof(raw),n,channel));
 meshtastic_Data d{};auto in=pb_istream_from_buffer(raw,n);assert(pb_decode(&in,meshtastic_Data_fields,&d)&&!in.bytes_left&&d.request_id==0x761);
 meshtastic_Position p{};auto pos=pb_istream_from_buffer(d.payload.bytes,d.payload.size);assert(pb_decode(&pos,meshtastic_Position_fields,&p)&&!pos.bytes_left);return p;
}
#include "channel_privacy/precision_cases.inc"
int main(){
 channelPrecisionCases();
 eventPrecisionCases();
 packetPrecisionCases();
 precision_cases();std::cout<<"PASS literal stock midpoint/zero whole-position encoding and precision clamp vectors\n";
 {
  uint8_t order[8];for(unsigned slot=0;slot<8;++slot){assert(mt::ChannelOrder::permutation(slot,order));for(unsigned i=0;i<8;++i)assert(order[i]==(i==0?slot:i==slot?0:i));}
  uint8_t before[8];memcpy(before,order,8);assert(!mt::ChannelOrder::permutation(8,order)&&!memcmp(before,order,8));assert(!mt::ChannelOrder::permutation(UINT32_MAX,order)&&!memcmp(before,order,8));
  std::cout<<"PASS all eight swap plans and invalid-index atomicity\n";
 }
 {
  const auto raw=read("v14-config.bin");assert(raw.size()==608&&raw[2]==14);mt::Config c;assert(mt::Config::decode(raw.data(),raw.size(),c)&&c.bytes[2]==mt::Config::Version&&c.primaryPositionPrecision()==15&&c.positionSharing());
  assert(!memcmp(c.bytes+3,raw.data()+3,119)&&!memcmp(c.bytes+123,raw.data()+123,483));
  auto bad=raw;bad[122]=1;const auto crc=mt::Config::checksum(bad.data(),606);bad[606]=uint8_t(crc);bad[607]=uint8_t(crc>>8);auto unchanged=c;assert(!mt::Config::decode(bad.data(),bad.size(),unchanged)&&unchanged.same(c));
  Backend b;b.records["mtbase/state0"]=read("v5-journal.bin");mt::SettingsJournal journal;assert(!journal.load(b)&&journal.config().same(c));
  assert(c.setPrimaryPositionPrecision(23)&&!journal.save(b,c));mt::SettingsJournal reboot;assert(!reboot.load(b)&&reboot.config().same(c));
  const auto old=journal.config();auto edit=old;edit.setTxEnabled(true);b.fail=true;assert(journal.save(b,edit)==-EIO&&journal.config().same(old)&&journal.state()==mt::SettingsJournal::State::IoError&&journal.save(b,old)==-EACCES);
  std::cout<<"PASS actual Config14/Journal5 producer migration, reserved-byte refusal and failed-save atomicity\n";
 }
 {
  auto c=initial();assert(c.setPrimaryPositionPrecision(32)&&c.positionPrecision(0)==15);auto p=response(c,0);assert(p.precision_bits==15&&p.latitude_i==123404288&&p.longitude_i==-123404288&&p.has_altitude&&p.altitude==-35);
  uint8_t key[16]{};key[0]=0x51;assert(c.setSecondaryChannel(7,channel(2,"Private",key,16,32))&&c.positionPrecision(7)==32);p=response(c,7);assert(p.precision_bits==32&&p.latitude_i==123456789&&p.longitude_i==-123456789);
  assert(c.setSecondaryChannel(6,channel(2,"Inherited",nullptr,0,31))&&c.positionPrecision(6)==15);p=response(c,6);assert(p.precision_bits==15);
  std::cout<<"PASS per-channel precision uses actual effective keys and caps public-key sharing\n";
 }
 {
  auto c=initial();uint8_t raw[128]{};size_t n=99;assert(!mt::PositionQuery::fixedResponse(c,1,raw,sizeof(raw),n)&&n==0);
  assert(c.setPrimaryPositionPrecision(15));assert(c.setFixedPosition(1,2,false,0)&&!c.positionSharing()&&c.positionPrecision(0)==0);n=99;assert(!mt::PositionQuery::fixedResponse(c,1,raw,sizeof(raw),n)&&n==0);
  assert(c.setPrimaryPositionPrecision(32));c.setPositionSharing(false);assert(!c.positionSharing()&&!c.primaryPositionPrecision());c.setPositionSharing(true);assert(c.primaryPositionPrecision()==15);
  const auto old=c;assert(!c.setPrimaryPositionPrecision(33)&&c.same(old));assert(c.setPrimaryPositionPrecision(1));n=99;assert(!mt::PositionQuery::fixedResponse(c,1,raw,sizeof(raw),n)&&n==0);
  std::cout<<"PASS zero consent, coordinate replacement, invalid precision and invalid geographic midpoint never disclose\n";
 }
 {
  auto c=initial();c.setPositionSharing(true);uint8_t privateKey[16]{};privateKey[0]=0x51;
  assert(c.setSecondaryChannel(3,channel(2,"Private",privateKey,16,32,true)));assert(c.setSecondaryChannel(4,channel(2,"Inherited",nullptr,0,8)));assert(c.setSecondaryChannel(5,channel(2,"Other",nullptr,0)));
  mt::PrimaryChannel::Selection before[8],after[8];for(unsigned i:{0u,3u,4u,5u})assert(c.channelSelection(i,before[i]));
  const auto old=c;assert(!c.promoteChannel(8)&&c.same(old));assert(!c.promoteChannel(2)&&c.same(old));assert(c.promoteChannel(3)&&c.valid());
  for(unsigned i:{0u,3u,4u,5u}){assert(c.channelSelection(i,after[i]));}
  assert(sameKey(before[0],after[3])&&sameKey(before[3],after[0])&&sameKey(before[4],after[4])&&sameKey(before[5],after[5]));
  assert(c.channelMuted()&&c.primaryPositionPrecision()==32&&c.positionPrecision(0)==32);mt::ChannelRecord inherited;assert(c.channelRecord(4,inherited)&&inherited.keySize()==16&&inherited.precision()==8);
  mt::Config reboot;assert(mt::Config::decode(c.bytes,sizeof(c.bytes),reboot)&&reboot.same(c));
  std::cout<<"PASS atomic promotion swaps metadata and materializes all inherited keys without changing untouched effective channels\n";
 }
 {
  auto c=initial();mt::ui::Model m;assert(mt::ui::radioView(c,false,true,m.radio)&&!strcmp(m.channelLabel(),"CHANNEL UNKNOWN"));
  assert(c.setPrimaryName("My Mesh")&&c.setLoRa(1,0)&&mt::ui::radioView(c,true,true,m.radio));assert(!strcmp(m.channelLabel(),"My Mesh")&&!strcmp(m.radio.modem,"LongSlow")&&m.radio.spreadingFactor==12&&m.radio.bandwidthHz==125000);
  auto request=c.radioRequest();request.usePreset=false;request.bandwidth=8;request.spreadingFactor=7;request.codingRate=8;request.overrideMHz=915.5f;request.offsetMHz=.25f;request.powerSpecified=true;request.power=0;
  assert(c.setRadio(request)&&mt::ui::radioView(c,true,true,m.radio)&&m.radio.frequencyHz==915750000&&m.radio.powerDbm==22&&!strcmp(m.radio.modem,"Custom"));char text[64];m.frequencyLabel(text,sizeof(text));assert(!strcmp(text,"915.750 MHz 22dBm"));m.modemLabel(text,sizeof(text));assert(!strcmp(text,"BW7.8 SF7 CR4/8"));
  const auto before=m.radio;assert(!mt::ui::set(m.radio,true,true,"name too long","modem",915000000,2));
  assert(m.radio.loaded==before.loaded&&m.radio.enabled==before.enabled&&m.radio.regionUS==before.regionUS&&!strcmp(m.radio.channel,before.channel)&&!strcmp(m.radio.modem,before.modem)&&m.radio.frequencyHz==before.frequencyHz&&m.radio.powerDbm==before.powerDbm&&m.radio.bandwidthHz==before.bandwidthHz&&m.radio.spreadingFactor==before.spreadingFactor&&m.radio.codingDenominator==before.codingDenominator);
  auto n=c.node();n.setKey(0,nullptr,0);c.node(n);assert(mt::ui::radioView(c,true,true,m.radio)&&!strcmp(m.channelLabel(),"CH0 DISABLED"));
  std::cout<<"PASS UI resolves custom names/presets/manual frequency/power and distinguishes unknown from disabled\n";
 }
}
