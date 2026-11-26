// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtNodeInfo.h"
#include "PrimaryChannel.hpp"
#include "RadioSettings.h"
#include "DeviceRolePolicy.h"
#include "HopPolicy.h"
#include "ChannelRecord.h"
#include "ChannelCandidates.h"

namespace mt {
// Explicit wire bytes, independent of compiler padding. Identity, private keys
// and route state are deliberately not part of this record. Byte 83 is a
// tri-state normal-use TX intent; bytes 84-96 hold a local fixed position and
// v5 bytes 97-100 hold the display carousel interval. V6 byte 101 stores the
// primary Public channel's local-attention mute flag. V9 byte 103 stores the
// long-name display preference. V10 stores modem preset at104 and little-endian
// channel number at105-106. V11 adds role107, rebroadcast108, a canonical
// NUL-padded primary name109-120. V12 adds hop limit121; reserved122-125
// and CRC126-127. V13 keeps bytes0-125, adds seven64-byte secondary
// records126-573 and CRC574-575, total576. V14 retains that body and
// appends mode, raw modem controls, regional-power intent, float MHz controls
// and a full32-bit channel number at574-605; CRC606-607, total608. V7 byte102
// is explicit fixed-position sharing(0/1). Runtime arm/queue state is volatile.
struct Config {
  static constexpr size_t LegacySize=80,PreviousSize=96,FormerSize=112,PrimarySize=128,BankSize=576,Size=608;
  static constexpr uint8_t Version=14;
  enum class TxIntent:uint8_t {Unspecified=0,Disabled=1,Enabled=2};
  enum PositionFlag:uint8_t {PositionValid=1,AltitudeValid=2};
  uint8_t bytes[Size]{};
  static uint16_t checksum(const uint8_t *p,size_t n) {
    uint16_t c=0xffff;
    for(size_t i=0;i<n;++i){c^=uint16_t(p[i])<<8;for(unsigned j=0;j<8;++j)c=c&0x8000?uint16_t((c<<1)^0x1021):uint16_t(c<<1);}
    return c;
  }
  Config() {
    bytes[0]='M';bytes[1]='T';bytes[2]=14;bytes[105]=20;bytes[600]=20;bytes[580]=11;bytes[584]=5;bytes[107]=1;bytes[121]=HopPolicy::Initial;
    bench::MtNodeRecord n;n.setName("ZephTastic",false);n.setName("ZT",true);n.setPublic();
    memcpy(bytes+6,n.bytes,72);
    // Preserve the previous prototype's default, while accepting stock's
    // explicit zero/default (60 seconds) and UINT32_MAX/always-on values.
    bytes[78]=15;resetSecondaryChannels();seal();
  }
  // Seven secondary slots share the same atomic Config journal as primary.
  // Primary remains slot0; changing its key also changes empty-key inheritance.
  void resetSecondaryChannels(){ChannelRecord empty;for(unsigned i=1;i<8;++i)memcpy(bytes+126+(i-1)*ChannelRecord::Size,empty.bytes,ChannelRecord::Size);}
  bool channelRecord(unsigned index,ChannelRecord &out)const {
    if(index>=8)return false;
    if(index){memcpy(out.bytes,bytes+126+(index-1)*ChannelRecord::Size,ChannelRecord::Size);return out.valid();}
    const auto record=node();
    return out.set(channelConfigured()?1:0,primaryName(),record.bytes+bench::MtNodeRecord::Key,record.bytes[3],
                   positionSharing()?PublicPositionPrecision:0,channelMuted());
  }
  bool channelSelection(unsigned index,PrimaryChannel::Selection &out)const {
    PrimaryChannel::clear(out);
    if(index>=8)return false;
    if(!index) {
      const auto record=node();return channelConfigured()&&PrimaryChannel::normalize(record.bytes+bench::MtNodeRecord::Key,record.bytes[3],out,effectivePrimaryName());
    }
    ChannelRecord stored;if(!channelRecord(index,stored)||stored.role()!=2)return false;
    const auto primary=node();const bool inherit=stored.keySize()==0;
    if(inherit&&!channelConfigured())return false;
    return PrimaryChannel::normalize(inherit?primary.bytes+bench::MtNodeRecord::Key:stored.key(),
      inherit?primary.bytes[3]:stored.keySize(),out,stored.name()[0]?stored.name():modemName());
  }
  bool setSecondaryChannel(unsigned index,const ChannelRecord &record) {
    if(!index||index>=8||!record.valid()||(record.role()!=0&&record.role()!=2))return false;
    // Network transports and secondary position sharing have no implemented
    // per-channel producer yet. Reject rather than acknowledge an inert flag.
    if(record.uplink()||record.downlink()||record.precision())return false;
    Config candidate=*this;
    memcpy(candidate.bytes+126+(index-1)*ChannelRecord::Size,record.bytes,ChannelRecord::Size);
    PrimaryChannel::Selection key;
    const bool ok=record.role()==0||candidate.channelSelection(index,key);PrimaryChannel::clear(key);
    if(!ok)return false;
    candidate.seal();*this=candidate;return true;
  }
  uint8_t channelCandidates(uint8_t wanted)const {
    uint8_t roles[8]{},hashes[8]{};
    for(unsigned i=0;i<8;++i){PrimaryChannel::Selection key;if(channelSelection(i,key)){roles[i]=uint8_t(i?2:1);hashes[i]=key.hash;}PrimaryChannel::clear(key);}
    return ChannelCandidates::mask(roles,hashes,wanted);
  }
  uint16_t crc() const {return checksum(bytes,Size-2);}
  void seal(){const auto c=crc();bytes[Size-2]=uint8_t(c);bytes[Size-1]=uint8_t(c>>8);}
  bench::MtNodeRecord node() const {bench::MtNodeRecord n;memcpy(n.bytes,bytes+6,72);return n;}
  void node(const bench::MtNodeRecord &n){memcpy(bytes+6,n.bytes,72);seal();}
  bool setNodeInfoSeconds(uint32_t seconds) {
    // Stock clamps Device writes to one hour. Existing shorter bench records
    // remain readable. V8 widens the interval range without changing layout.
    if(seconds>bench::MtNodeRecord::MaxIntervalSeconds)return false;
    if(seconds<3600)seconds=3600;
    auto n=node();n.write32(bench::MtNodeRecord::Interval,seconds);n.seal();node(n);return true;
  }
  uint32_t displaySeconds() const {
    return uint32_t(bytes[78])|(uint32_t(bytes[79])<<8)|(uint32_t(bytes[80])<<16)|(uint32_t(bytes[81])<<24);
  }
  bool displayFlip() const {return bytes[82]!=0;}
  // V9 adds a persisted display preference; old records migrate to the
  // protobuf default (short names). No NodeDB identity is rewritten.
  bool displayLongNames()const{return bytes[103]!=0;}
  void setDisplayLongNames(bool enabled){bytes[103]=enabled;seal();}
  uint32_t displayCarouselSeconds() const {return get32(bytes+97);}
  static constexpr uint32_t PublicPositionPrecision=15;
  bool positionSharing()const{return bytes[102]==1;}
  void setPositionSharing(bool enabled){bytes[102]=enabled&&fixedPosition();seal();}
  bool channelMuted()const{return bytes[101]!=0;}
  TxIntent txIntent()const{return static_cast<TxIntent>(bytes[83]);}
  bool txIntentSpecified()const{return txIntent()!=TxIntent::Unspecified;}
  bool txEnabled(bool defaultEnabled=false)const{
    return txIntent()==TxIntent::Enabled||(txIntent()==TxIntent::Unspecified&&defaultEnabled);
  }
  void setTxEnabled(bool enabled){bytes[83]=uint8_t(enabled?TxIntent::Enabled:TxIntent::Disabled);seal();}
  static bool validDisplaySeconds(uint32_t s){return s<=86400||s==UINT32_MAX;}
  static bool validCarouselSeconds(uint32_t s){return s<=86400;}
  bool setDisplay(uint32_t seconds,bool flip,uint32_t carousel){
    if(!validDisplaySeconds(seconds)||!validCarouselSeconds(carousel))return false;
    for(unsigned i=0;i<4;++i)bytes[78+i]=uint8_t(seconds>>(i*8));
    bytes[82]=flip;put32(bytes+97,carousel);seal();return true;
  }
  bool setDisplay(uint32_t seconds,bool flip){return setDisplay(seconds,flip,displayCarouselSeconds());}
  void setChannelMuted(bool muted){bytes[101]=muted;seal();}
  unsigned modemPreset()const{return bytes[104];}
  uint32_t channelNumber()const{return bytes[2]>=14?get32(bytes+600):uint16_t(bytes[105])|(uint16_t(bytes[106])<<8);}
  bool usePreset()const{return bytes[2]<14||!(bytes[574]&1);}
  bool powerSpecified()const{return bytes[2]>=14&&(bytes[574]&2);}
  const char *modemName()const{return usePreset()?PrimaryChannel::presetName(modemPreset()):"Custom";}
  static float getFloat32(const uint8_t *p){const auto bits=get32(p);float value;static_assert(sizeof(value)==sizeof(bits));memcpy(&value,&bits,sizeof(value));return value;}
  static void putFloat32(uint8_t *p,float value){uint32_t bits;memcpy(&bits,&value,sizeof(bits));put32(p,bits);}
  RadioSettings::Request radioRequest()const {
    RadioSettings::Request r;r.usePreset=usePreset();r.preset=modemPreset();r.channel=channelNumber();r.powerSpecified=powerSpecified();
    if(bytes[2]>=14){r.bandwidth=get32(bytes+576);r.spreadingFactor=get32(bytes+580);r.codingRate=get32(bytes+584);r.power=getSigned32(bytes+588);r.overrideMHz=getFloat32(bytes+592);r.offsetMHz=getFloat32(bytes+596);}return r;
  }
  bool resolveRadio(LoRaPresets::Profile &p,USChannelSelection &s)const{return RadioSettings::resolve(radioRequest(),p,s,effectivePrimaryName());}
  int effectivePower(bool t096)const{return RadioSettings::effectivePower(radioRequest(),t096);}
  bool setRadio(const RadioSettings::Request &input) {
    auto r=input;
    LoRaPresets::Profile p{};USChannelSelection s{};
    const char *name=bytes[109]?primaryName():(r.usePreset?PrimaryChannel::presetName(r.preset):"Custom");
    if(!RadioSettings::resolve(r,p,s,name))return false;
    const char *defaultName=r.usePreset?PrimaryChannel::presetName(r.preset):"Custom";
    if(!r.overrideMHz&&r.channel>s.slotCount&&(r.usePreset||strcmp(name,defaultName)))r.channel=uint32_t(s.slot)+1;
    Config candidate=*this;candidate.bytes[2]=14;candidate.bytes[104]=uint8_t(r.preset);candidate.bytes[105]=uint8_t(r.channel);candidate.bytes[106]=uint8_t(r.channel>>8);
    candidate.bytes[574]=uint8_t((r.usePreset?0:1)|(r.powerSpecified?2:0));candidate.bytes[575]=0;
    put32(candidate.bytes+576,r.bandwidth);put32(candidate.bytes+580,r.spreadingFactor);put32(candidate.bytes+584,r.codingRate);
    putSigned32(candidate.bytes+588,r.powerSpecified?ManualLoRa::regionalPower(r.power,30,false):0);
    putFloat32(candidate.bytes+592,r.overrideMHz);putFloat32(candidate.bytes+596,r.offsetMHz);put32(candidate.bytes+600,r.channel);candidate.bytes[604]=candidate.bytes[605]=0;
    auto record=candidate.node();if(!record.valid())return false;
    if(record.bytes[3]){PrimaryChannel::Selection key;if(!PrimaryChannel::normalize(record.bytes+bench::MtNodeRecord::Key,record.bytes[3],key,candidate.effectivePrimaryName()))return false;record.setKey(key.hash,key.bytes,key.length);PrimaryChannel::clear(key);candidate.node(record);}
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  unsigned hopLimit()const{return bytes[121];}
  bool setHopLimit(unsigned value){if(!HopPolicy::valid(value))return false;bytes[121]=uint8_t(value);seal();return true;}
  unsigned role()const{return bytes[107];}
  unsigned rebroadcastMode()const{return bytes[108];}
  bool setDeviceRole(uint32_t roleValue,uint32_t mode) {
    if(!DeviceRolePolicy::valid(roleValue,mode))return false;
    bytes[107]=uint8_t(roleValue);bytes[108]=uint8_t(mode);seal();return true;
  }
  const char *primaryName()const{return reinterpret_cast<const char *>(bytes+109);}
  const char *effectivePrimaryName()const {
    return bytes[109]?primaryName():modemName();
  }
  bool canonicalPrimaryName()const {
    bool terminated=false;
    for(unsigned i=109;i<121;++i) {
      const auto ch=bytes[i];
      if(!ch)terminated=true;
      else if(terminated||ch<0x20||ch>0x7e)return false;
    }
    return terminated;
  }
  bool channelConfigured()const {
    return canonicalPrimaryName()&&PrimaryChannel::configured(node(),effectivePrimaryName());
  }
  bool setPrimaryName(const char *name) {
    if(!PrimaryChannel::validName(name))return false;
    Config candidate=*this;memset(candidate.bytes+109,0,12);for(unsigned i=0;name[i];++i)candidate.bytes[109+i]=uint8_t(name[i]);
    if(!candidate.setRadio(candidate.radioRequest()))return false;
    *this=candidate;return true;
  }
  bool setLoRa(unsigned preset,uint32_t channel) {
    auto request=radioRequest();request.usePreset=true;request.preset=preset;request.channel=channel;
    return setRadio(request);
  }
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static void put32(uint8_t *p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));
  }
  static int32_t getSigned32(const uint8_t *p) {
    const uint32_t raw=get32(p);int32_t value;memcpy(&value,&raw,sizeof(value));return value;
  }
  static void putSigned32(uint8_t *p,int32_t value) {
    uint32_t raw;memcpy(&raw,&value,sizeof(raw));put32(p,raw);
  }
  bool fixedPosition()const{return (bytes[84]&PositionValid)!=0;}
  bool fixedAltitude()const{return fixedPosition()&&(bytes[84]&AltitudeValid)!=0;}
  int32_t latitudeI()const{return getSigned32(bytes+85);}
  int32_t longitudeI()const{return getSigned32(bytes+89);}
  int32_t altitude()const{return getSigned32(bytes+93);}
  bool setFixedPosition(int32_t latitude,int32_t longitude,bool hasAltitude,int32_t altitudeMetres) {
    if(latitude<-900000000||latitude>900000000||longitude<-1800000000||longitude>1800000000)return false;
    bytes[102]=0; // A new coordinate always requires fresh explicit consent.
    bytes[84]=PositionValid|(hasAltitude?AltitudeValid:0);
    putSigned32(bytes+85,latitude);putSigned32(bytes+89,longitude);
    putSigned32(bytes+93,hasAltitude?altitudeMetres:0);seal();return true;
  }
  void clearFixedPosition(){bytes[102]=0;memset(bytes+84,0,13);seal();}
  bool validBody(uint8_t version) const {
    if(version<4||version>14)return false;
    const size_t recordSize=version>=14?Size:version>=13?BankSize:version>=11?PrimarySize:FormerSize;
    if(bytes[0]!='M'||bytes[1]!='T'||bytes[2]!=version||bytes[3]||bytes[4]>1||bytes[5]||
       !node().valid()||(version<8&&node().interval()>86400)||!validDisplaySeconds(displaySeconds())||bytes[82]>1||bytes[83]>2)return false;
    if(bytes[84]&uint8_t(~(PositionValid|AltitudeValid)))return false;
    if(!fixedPosition()) {
      for(unsigned i=84;i<97;++i)if(bytes[i])return false;
    } else {
      if(latitudeI()<-900000000||latitudeI()>900000000||longitudeI()<-1800000000||longitudeI()>1800000000)return false;
      if(!fixedAltitude())for(unsigned i=93;i<97;++i)if(bytes[i])return false;
    }
    if(version==4){for(unsigned i=97;i<recordSize-2;++i)if(bytes[i])return false;}
    else if(version==5) {
      if(!validCarouselSeconds(displayCarouselSeconds()))return false;
      for(unsigned i=101;i<recordSize-2;++i)if(bytes[i])return false;
    } else {
      if(!validCarouselSeconds(displayCarouselSeconds())||bytes[101]>1)return false;
      if(version==6&&bytes[102])return false;
      if(bytes[102]>1||(bytes[102]&&!fixedPosition()))return false;
      if(bytes[103]>1||(version<9&&bytes[103]))return false;
      if(version<10){for(unsigned i=104;i<recordSize-2;++i)if(bytes[i])return false;}
      else {
        LoRaPresets::Profile p{};USChannelSelection s{};
        if(version>=11) {
          if(!DeviceRolePolicy::valid(role(),rebroadcastMode())||!canonicalPrimaryName()||
             !(version>=14?resolveRadio(p,s):RadioSettings::resolve(modemPreset(),channelNumber(),p,s,effectivePrimaryName())))return false;
          if(version>=12&&!HopPolicy::valid(hopLimit()))return false;
          if(version>=14&&((bytes[574]&uint8_t(~3u))||bytes[575]||bytes[604]||bytes[605]||(!powerSpecified()&&get32(bytes+588))||
             uint16_t(channelNumber())!=(uint16_t(bytes[105])|(uint16_t(bytes[106])<<8))))return false;
          for(unsigned i=version>=12?122:121;i<(version>=13?126:recordSize-2);++i)if(bytes[i])return false;
          if(version>=13)for(unsigned i=1;i<8;++i){ChannelRecord c;if(!channelRecord(i,c)||(c.role()!=0&&c.role()!=2)||c.uplink()||c.downlink()||c.precision())return false;PrimaryChannel::Selection key;const bool usable=c.role()==0||channelSelection(i,key);PrimaryChannel::clear(key);if(!usable)return false;}
        } else {
          if(!RadioSettings::resolve(modemPreset(),channelNumber(),p,s,"LongFast"))return false;
          for(unsigned i=107;i<recordSize-2;++i)if(bytes[i])return false;
        }
      }
    }
    return (uint16_t(bytes[recordSize-2])|(uint16_t(bytes[recordSize-1])<<8))==checksum(bytes,recordSize-2);
  }
  bool valid() const {return validBody(14);}
  bool provisioned() const {return valid()&&bytes[4]==1&&channelConfigured();}
  bool same(const Config &other) const {return !memcmp(bytes,other.bytes,Size);}
  // Exact persisted semantics outside bytes 84-96. CRC bytes are derived and
  // therefore excluded. Callers validate both records before using this to
  // select the narrow live position publication path.
  bool sameExceptPosition(const Config &other) const {
    return !memcmp(bytes,other.bytes,84)&&!memcmp(bytes+97,other.bytes+97,Size-2-97);
  }
  // Exact persisted semantics outside the v6 channel-mute byte. The derived
  // CRC is excluded. SettingsEdit uses this only after validating both records
  // to publish a mute-only save without resetting protocol/routing histories.
  bool sameExceptChannelMute(const Config &other) const {
    return !memcmp(bytes,other.bytes,101)&&!memcmp(bytes+102,other.bytes+102,Size-2-102);
  }
  static bool decode(const uint8_t *p,size_t n,Config &out){
    if(!p)return false;
    Config candidate;
    if(n==Size){
      memcpy(candidate.bytes,p,Size);
      if(p[2]!=14||!candidate.validBody(14))return false;
    }
    else if(n==BankSize){
      memcpy(candidate.bytes,p,BankSize);if(p[2]!=13||!candidate.validBody(13))return false;
    }
    else if(n==PrimarySize){
      memcpy(candidate.bytes,p,PrimarySize);
      if((p[2]!=11&&p[2]!=12)||!candidate.validBody(p[2]))return false;
    }
    else if(n==FormerSize){
      memcpy(candidate.bytes,p,FormerSize);
      if(p[2]<4||p[2]>10||!candidate.validBody(p[2]))return false;
      if(p[2]<10){candidate.bytes[104]=0;candidate.bytes[105]=20;candidate.bytes[106]=0;}
    }
    else if(n==PreviousSize){
      if(p[0]!='M'||p[1]!='T'||(p[2]!=2&&p[2]!=3)||p[3]||p[4]>1||p[5]||
         !validDisplaySeconds(uint32_t(p[78])|(uint32_t(p[79])<<8)|(uint32_t(p[80])<<16)|(uint32_t(p[81])<<24))||p[82]>1||
         (uint16_t(p[94])|(uint16_t(p[95])<<8))!=checksum(p,94))return false;
      memcpy(candidate.bytes,p,84);
      if(!candidate.node().valid()||candidate.node().interval()>86400)return false;
      if(p[2]==2){for(unsigned i=83;i<94;++i)if(p[i])return false;candidate.bytes[83]=uint8_t(TxIntent::Unspecified);}
      else {if(p[83]>2)return false;for(unsigned i=84;i<94;++i)if(p[i])return false;}
      memset(candidate.bytes+84,0,Size-84);candidate.bytes[105]=20;
    }
    else if(n==LegacySize){
      if(p[0]!='M'||p[1]!='T'||p[2]!=1||p[3]||p[4]>1||p[5]||
         (uint16_t(p[78])|(uint16_t(p[79])<<8))!=checksum(p,78))return false;
      memcpy(candidate.bytes,p,78);candidate.bytes[83]=uint8_t(TxIntent::Unspecified);
    }else return false;
    if(p[2]<8&&candidate.node().interval()>86400)return false;
    if(n!=Size&&n!=BankSize&&n!=PrimarySize) {
      candidate.bytes[2]=14;memset(candidate.bytes+107,0,126-107);candidate.bytes[107]=1;candidate.bytes[121]=HopPolicy::Initial;
      // Old nondefault presets still used LongFast for both crypto and slot.
      if(candidate.modemPreset())memcpy(candidate.bytes+109,"LongFast",9);
      candidate.resetSecondaryChannels();candidate.seal();
    }
    if(n==PrimarySize){candidate.bytes[2]=14;if(p[2]==11)candidate.bytes[121]=HopPolicy::Initial;candidate.resetSecondaryChannels();candidate.seal();}
    if(n!=Size){const uint32_t channel=uint16_t(candidate.bytes[105])|(uint16_t(candidate.bytes[106])<<8);candidate.bytes[2]=14;memset(candidate.bytes+574,0,Size-574);put32(candidate.bytes+580,11);put32(candidate.bytes+584,5);put32(candidate.bytes+600,channel);candidate.seal();}
    if(!candidate.valid())return false;
    out=candidate;return true;
  }
};
static_assert(sizeof(Config)==Config::Size);
}
