// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtNodeInfo.h"
#include "PrimaryChannel.hpp"
#include "RadioSettings.h"
#include "DeviceRolePolicy.h"
#include "HopPolicy.h"
#include "ChannelRecord.h"
#include "ChannelCandidates.h"
#include "ChannelOrder.h"
#include "gnss/GnssSettings.h"

#if defined(CONFIG_MT_LOCAL_CLOCK)
#include "clock/LocalTime.h"
#endif

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
  static constexpr size_t LegacySize=80,PreviousSize=96,FormerSize=112,PrimarySize=128,BankSize=576,RadioSize=608,Size=640;
  // V15 uses previously reserved byte122 for primary precision; layout/CRC
  // and journal envelope stay the same size. V14 is validated before migration.
// V16 retains the complete V15 body through byte605. A32-byte GNSS
  // settings record occupies606..637; the outer CRC moves to638..639.
// V17 uses reserved byte123 for primary MQTT downlink consent and permits the
// existing secondary ChannelRecord downlink bit. V18 reinterprets legacy
// boolean byte4 as the stock RegionCode enum; the old 0/1 meanings remain
// UNSET/US. V19 uses reserved header byte5 for owner.is_unmessagable and
// permits measured heading / unknown PDOP selection. V20 uses bits0-1 of
// reserved byte125 for licensed-operation and duty-cycle-override intent.
// V21 uses bit4 for managed mode and bit5 for serial-console disabled; bit5
// clear preserves the stock serial-enabled default. V20 still validates bits
// 4-7 as zero before migration. V22 uses bits6/7 for debug-log API and
// opt-in legacy Admin-channel controls; V21 validates those bits as zero
// before migration. V23 uses byte604 for primary AEAD; secondary records use
// ChannelRecord flag bit3. V22 validates both locations as reserved. Size and
// framing stay fixed.
  // V24 assigns the first secondary slot's previously zero padding (bytes
  // 174..186) to the three ignored LoRa sender IDs. The channel's normalized bytes
  // and its existing CRC stay independent; the outer Config CRC covers both.
#if defined(CONFIG_MT_LOCAL_CLOCK)
  // V25 occupies 65 bytes of normalized channel-record padding across slots
  // 2..6. The channel key and CRC stay canonical and the core/backup size stays fixed.
  static constexpr uint8_t Version=25;
#else
  static constexpr uint8_t Version=24;
#endif
  static constexpr unsigned IgnoreLimit=3;
  static constexpr size_t ChannelTableOffset=126, IgnoreSlotOffset=48;
  static constexpr size_t IgnoreBytes=1+4*IgnoreLimit;
  static constexpr size_t IgnoreCountOffset=ChannelTableOffset+IgnoreSlotOffset, IgnoreIdsOffset=IgnoreCountOffset+1;
  static_assert(IgnoreSlotOffset+IgnoreBytes<=ChannelRecord::Size-2);
  unsigned ignoredSenderCount()const{return bytes[IgnoreCountOffset];}
  uint32_t ignoredSender(unsigned index)const{return get32(bytes+IgnoreIdsOffset+4*index);}
  bool ignoresIncoming(uint32_t sender)const{
    for(unsigned i=0;i<ignoredSenderCount()&&i<IgnoreLimit;++i)
      if(ignoredSender(i)==sender)return true;
    return false;
  }
  bool sameExceptIgnoredSenders(const Config &other)const {
    constexpr size_t after=IgnoreCountOffset+IgnoreBytes;
    return !memcmp(bytes,other.bytes,IgnoreCountOffset)&&!memcmp(bytes+after,other.bytes+after,Size-2-after);
  }
  bool setIgnoredSenders(const uint32_t *senders,unsigned count){
    if(count>IgnoreLimit||(!senders&&count))return false;
    bytes[IgnoreCountOffset]=uint8_t(count);
    for(unsigned i=0;i<IgnoreLimit;++i)put32(bytes+IgnoreIdsOffset+4*i,i<count?senders[i]:0);
    seal();return true;
  }
#if defined(CONFIG_MT_LOCAL_CLOCK)
  static constexpr unsigned ClockBytes=65,ClockChunk=ChannelRecord::Size-2-IgnoreSlotOffset;
  static constexpr size_t clockOffset(unsigned position){
    return ChannelTableOffset+(1+position/ClockChunk)*ChannelRecord::Size+IgnoreSlotOffset+position%ClockChunk;
  }
  static constexpr unsigned clockPadding(unsigned slot){
    if(slot<2||slot>6)return 0;
    const unsigned remaining=ClockBytes-(slot-2)*ClockChunk;
    return remaining<ClockChunk?remaining:ClockChunk;
  }
  void timezone(char (&out)[65])const {
    for(unsigned i=0;i<64;++i)out[i]=char(bytes[clockOffset(i)]);
    out[64]=0;
  }
  bool hasClockPreferences()const{return bytes[clockOffset(0)]||bytes[clockOffset(64)];}
  bool twelveHourClock()const{return bytes[clockOffset(64)]==1;}
  void twelveHourClock(bool enabled){bytes[clockOffset(64)]=enabled;bytes[2]=Version;seal();}
  bool setTimezone(const char (&zone)[65]){
    const size_t n=strnlen(zone,sizeof(zone));
    if(n==sizeof(zone)||!clock::LocalTime::valid(zone))return false;
    for(unsigned i=0;i<64;++i)bytes[clockOffset(i)]=i<n?uint8_t(zone[i]):0;
    bytes[2]=Version;seal();return true;
  }
  bool timezoneEquals(const char (&zone)[65])const{
    char current[65];timezone(current);return !strncmp(current,zone,sizeof(current));
  }
  bool sameExceptClock(const Config &other)const{return sameNonClockBytes(other.bytes);}
  bool sameNonClockBytes(const uint8_t *other)const{
    if(memcmp(bytes,other,2))return false;
    size_t cursor=3; // Canonical V24/V25 depends on the clock fields.
    for(unsigned slot=2;slot<=6;++slot){
      const size_t begin=ChannelTableOffset+(slot-1)*ChannelRecord::Size+IgnoreSlotOffset;
      if(memcmp(bytes+cursor,other+cursor,begin-cursor))return false;
      cursor=begin+clockPadding(slot);
    }
    return !memcmp(bytes+cursor,other+cursor,Size-2-cursor);
  }
  bool validClock()const{
    char zone[65];timezone(zone);
    const size_t n=strlen(zone);
    for(unsigned i=n;i<64;++i)if(zone[i])return false;
    return bytes[clockOffset(64)]<=1&&clock::LocalTime::valid(zone);
  }
  uint32_t localClock(uint32_t utc)const{
    if(!utc||!bytes[clockOffset(0)])return utc;
    char zone[65];timezone(zone);return clock::LocalTime::project(zone,utc);
  }
#endif
  void storeSecondaryRecord(unsigned index,const ChannelRecord &record) {
    uint8_t *slot=bytes+ChannelTableOffset+(index-1)*ChannelRecord::Size;
    unsigned keep=index==1?IgnoreBytes:0;
#if defined(CONFIG_MT_LOCAL_CLOCK)
    if(index>1&&bytes[2]>=25)keep=clockPadding(index);
#endif
    if(keep) {
      const size_t tail=IgnoreSlotOffset+keep;
      memcpy(slot,record.bytes,IgnoreSlotOffset);
      memcpy(slot+tail,record.bytes+tail,ChannelRecord::Size-tail);
    } else memcpy(slot,record.bytes,ChannelRecord::Size);
  }
  enum class TxIntent:uint8_t {Unspecified=0,Disabled=1,Enabled=2};
  enum PositionFlag:uint8_t {PositionValid=1,AltitudeValid=2};
  uint8_t bytes[Size]{};
  static uint16_t checksum(const uint8_t *p,size_t n) {
    uint16_t c=0xffff;
    for(size_t i=0;i<n;++i){c^=uint16_t(p[i])<<8;for(unsigned j=0;j<8;++j)c=c&0x8000?uint16_t((c<<1)^0x1021):uint16_t(c<<1);}
    return c;
  }
  Config() {
    bytes[0]='M';bytes[1]='T';bytes[2]=Version;bytes[105]=20;bytes[600]=20;bytes[580]=11;bytes[584]=5;bytes[107]=1;bytes[121]=HopPolicy::Initial;
    bench::MtNodeRecord n;n.setName("ZephTastic",false);n.setName("ZT",true);n.setPublic();
    memcpy(bytes+6,n.bytes,72);
    // Preserve the previous prototype's default, while accepting stock's
    // explicit zero/default (60 seconds) and UINT32_MAX/always-on values.
    bytes[78]=15;resetSecondaryChannels();seal();
  }
  // Seven secondary slots share the same atomic Config journal as primary.
  // Primary remains slot0; changing its key also changes empty-key inheritance.
  void resetSecondaryChannels(){ChannelRecord empty;for(unsigned i=1;i<8;++i)storeSecondaryRecord(i,empty);}
  bool channelRecord(unsigned index,ChannelRecord &out)const {
    if(index>=8)return false;
    if(index){memcpy(out.bytes,bytes+ChannelTableOffset+(index-1)*ChannelRecord::Size,ChannelRecord::Size);
      if(bytes[2]>=24&&index==1)memset(out.bytes+IgnoreSlotOffset,0,IgnoreBytes);
#if defined(CONFIG_MT_LOCAL_CLOCK)
      if(bytes[2]>=25&&index>1)memset(out.bytes+IgnoreSlotOffset,0,clockPadding(index));
#endif
      return out.valid();}
    const auto record=node();
    return out.set(channelConfigured()?1:0,primaryName(),record.bytes+bench::MtNodeRecord::Key,record.bytes[3],
                   primaryPositionPrecision(),channelMuted(),false,primaryDownlinkEnabled(),primaryAead());
  }
  bool primaryDownlinkEnabled()const{return bytes[2]>=17&&bytes[123]==1;}
  bool primaryAead()const{return bytes[2]>=23&&bytes[604]==1;}
  bool setPrimaryAead(bool enabled) {
    if(!valid()||(enabled&&(licensedOperation()||!channelConfigured())))return false;
    Config candidate=*this;candidate.bytes[2]=Version;candidate.bytes[604]=enabled?1:0;candidate.bytes[605]=0;
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool ignoreMqtt()const{return bytes[2]>=17&&bytes[124]==1;}
  bool channelDownlinkEnabled(unsigned index)const {
    ChannelRecord record;return channelRecord(index,record)&&record.role()!=0&&record.downlink();
  }
  bool anyChannelDownlinkEnabled()const {
    for(unsigned i=0;i<8;++i)if(channelDownlinkEnabled(i))return true;
    return false;
  }
  bool setPrimaryDownlinkEnabled(bool enabled) {
    if(!valid()||(enabled&&!channelConfigured()))return false;Config candidate=*this;candidate.bytes[2]=Version;
    candidate.bytes[123]=enabled?1:0;candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setIgnoreMqtt(bool enabled) {
    if(!valid())return false;Config candidate=*this;candidate.bytes[2]=Version;
    candidate.bytes[124]=enabled?1:0;candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool channelSelection(unsigned index,PrimaryChannel::Selection &out)const {
    PrimaryChannel::clear(out);
    if(index>=8)return false;
    if(!index) {
      const auto record=node();return channelConfigured()&&PrimaryChannel::fromRecord(record,effectivePrimaryName(),out,primaryAead());
    }
    ChannelRecord stored;if(!channelRecord(index,stored)||stored.role()!=2)return false;
    const auto primary=node();const bool inherit=stored.keySize()==0;
    const char *name=stored.name()[0]?stored.name():modemName();
    if(licensedOperation()) {
      if(!inherit||stored.aead())return false;
      return PrimaryChannel::normalizeLicensedPlaintext(name,out);
    }
    if(inherit&&!channelConfigured())return false;
    return PrimaryChannel::normalize(inherit?primary.bytes+bench::MtNodeRecord::Key:stored.key(),
      inherit?primary.bytes[3]:stored.keySize(),out,name,stored.aead());
  }
  // Stable identity of an RF channel. Unrelated preferences and representation
  // details such as an inherited rather than explicit identical PSK are omitted.
  struct DomainChannel {
    uint8_t enabled, hash, length, licensedPlaintext, aead, key[32];
    char name[12];
    ~DomainChannel(){bench::wipeKeyBuffer(key,sizeof(key));}
  };
  void readDomainChannel(unsigned slot,DomainChannel &out)const {
    PrimaryChannel::Selection key{};
    ChannelRecord record;
    if(channelSelection(slot,key)&&channelRecord(slot,record)){
      out.enabled=1;out.hash=key.hash;out.length=uint8_t(key.length);
      out.licensedPlaintext=key.licensedPlaintext;out.aead=key.aead;
      memcpy(out.key,key.bytes,key.length);
      const char *name=record.name()[0]?record.name():modemName();
      memcpy(out.name,name,strlen(name)); // Both canonical names and stock preset names fit in 11 bytes.
    }
    PrimaryChannel::clear(key);bench::wipeKeyBuffer(&record,sizeof(record));
  }
  uint8_t channelDomainFlags()const{return uint8_t(provisioned())|(uint8_t(licensedOperation())<<1);}
  bool sameChannelDomain(const Config &other)const {
    if(channelDomainFlags()!=other.channelDomainFlags())return false;
    for(unsigned i=0;i<8;++i){
      DomainChannel a{},b{};readDomainChannel(i,a);other.readDomainChannel(i,b);
      if(memcmp(&a,&b,sizeof(a)))return false;
    }
    return true;
  }
  bool setSecondaryChannel(unsigned index,const ChannelRecord &record) {
    if(!index||index>=8||!record.valid()||(record.role()!=0&&record.role()!=2)||(record.role()==0&&record.aead()))return false;
    if(record.uplink()||(record.precision()&&!positionPotential()))return false;
    Config candidate=*this;
    candidate.storeSecondaryRecord(index,record);
    PrimaryChannel::Selection key;
    const bool ok=record.role()==0||candidate.channelSelection(index,key);PrimaryChannel::clear(key);
    if(!ok)return false;
    if(record.role()==2&&record.precision())candidate.bytes[102]=1; // Explicit local sharing edit.
    bool sharingConfigured=candidate.primaryPositionPrecision()!=0;
    for(unsigned i=1;i<8;++i){ChannelRecord c;if(candidate.channelRecord(i,c)&&c.role()==2&&c.precision())sharingConfigured=true;}
    if(!sharingConfigured)candidate.bytes[102]=0;
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  uint8_t channelCandidates(uint8_t wanted)const {
    uint8_t roles[8]{},hashes[8]{};
    for(unsigned i=0;i<8;++i){PrimaryChannel::Selection key;if(channelSelection(i,key)){roles[i]=uint8_t(i?2:1);hashes[i]=key.hash;}PrimaryChannel::clear(key);}
    return ChannelCandidates::mask(roles,hashes,wanted);
  }
  uint16_t crc() const {return checksum(bytes,Size-2);}
  void seal(){
#if defined(CONFIG_MT_LOCAL_CLOCK)
    // Retain byte-exact V24 across other saves, FLASH backups and new remote
    // transactions until a local clock preference actually needs V25.
    if(bytes[2]==25&&!hasClockPreferences())bytes[2]=24;
#endif
    const auto c=crc();bytes[Size-2]=uint8_t(c);bytes[Size-1]=uint8_t(c>>8);}
  bench::MtNodeRecord node() const {bench::MtNodeRecord n;memcpy(n.bytes,bytes+6,72);return n;}
  void node(const bench::MtNodeRecord &n){
    memcpy(bytes+6,n.bytes,72);
    // Primary AEAD is meaningful only with an effective keyed channel. Keep
    // the mode across valid key/name edits, but normalize a keyless or licensed
    // node transition exactly as stock Channels::fixupChannel does.
    if(bytes[2]>=23&&(n.licensedPlaintext()||(n.bytes[3]!=16&&n.bytes[3]!=32)))bytes[604]=0;
    seal();
  }
  bool ownerUnmessagable()const{return bytes[2]>=19&&bytes[5]==1;}
  void setOwnerUnmessagable(bool value){bytes[5]=value?1:0;seal();}
  bool licensedOperation()const{return bytes[2]>=20&&(bytes[125]&1u)!=0;}
  bool overrideDutyCycle()const{return bytes[2]>=20&&(bytes[125]&2u)!=0;}
  uint32_t packetSignaturePolicy()const{return bytes[2]>=20?uint32_t((bytes[125]>>2)&3u):0u;}
  bool isManaged()const{return bytes[2]>=21&&(bytes[125]&0x10u)!=0;}
  bool serialEnabled()const{return bytes[2]<21||(bytes[125]&0x20u)==0;}
  bool debugLogApiEnabled()const{return bytes[2]>=22&&(bytes[125]&0x40u)!=0;}
  bool adminChannelEnabled()const{return bytes[2]>=22&&(bytes[125]&0x80u)!=0;}
  // Persistence codec only. The owner must require an existing keyed channel
  // named "admin" before enabling legacy channel administration.
  bool setSecurityOptions(bool debugLogApiEnabled,bool adminChannelEnabled) {
    if(!valid()||(adminChannelEnabled&&licensedOperation()))return false;
    Config candidate=*this;candidate.bytes[2]=Version;
    candidate.bytes[125]=uint8_t(candidate.bytes[125]&uint8_t(~0xc0u));
    if(debugLogApiEnabled)candidate.bytes[125]|=0x40u;
    if(adminChannelEnabled)candidate.bytes[125]|=0x80u;
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  // Persistence codec only: the owner must separately validate the current
  // Admin allowlist and a usable authenticated recovery route before enabling.
  bool setSecurityControls(bool managed,bool serialEnabled) {
    if(!valid())return false;
    Config candidate=*this;candidate.bytes[2]=Version;
    candidate.bytes[125]=uint8_t(candidate.bytes[125]&uint8_t(~0x30u));
    if(managed)candidate.bytes[125]|=0x10u;
    if(!serialEnabled)candidate.bytes[125]|=0x20u;
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setLicensedOperation(bool enabled) {
    if(!valid())return false;
    if(enabled)return licensedOperation(); // Enabling requires the atomic ham operation below.
    if(!licensedOperation())return true;
    Config candidate=*this;candidate.bytes[2]=Version;candidate.bytes[125]&=uint8_t(~3u);
    auto record=candidate.node();record.setKey(0,nullptr,0);candidate.node(record);
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setOverrideDutyCycle(bool enabled) {
    if(!valid()||(enabled&&!licensedOperation()))return false;Config candidate=*this;candidate.bytes[2]=Version;
    candidate.bytes[125]=uint8_t((candidate.bytes[125]&~2u)|(enabled?2u:0u));
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setPacketSignaturePolicy(uint32_t policy) {
    if(policy>2||!valid())return false;Config candidate=*this;candidate.bytes[2]=Version;
    candidate.bytes[125]=uint8_t((candidate.bytes[125]&~0x0cu)|(policy<<2));
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  bool setNodeInfoSeconds(uint32_t seconds) {
    // Stock clamps Device writes to one hour. Existing shorter bench records
    // remain readable. V8 widens the interval range without changing layout.
    if(!bench::MtNodeRecord::intervalSupported(seconds))return false;
    if(seconds<3600)seconds=3600;
    Config candidate=*this;auto n=candidate.node();n.write32(bench::MtNodeRecord::Interval,seconds);n.seal();candidate.node(n);
    if(licensedOperation()&&!candidate.valid())return false;*this=candidate;return true;
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
  GnssSettings gnssSettings()const {
    GnssSettings out;
    if(bytes[2]>=16)(void)GnssSettings::decode(bytes+606,GnssSettings::Size,out);
    return out;
  }
  bool positionPotential()const{return fixedPosition()||(bytes[2]>=16&&(bytes[606]&1));}
  static bool supportedGnssSettings(const GnssSettings &settings) {
    // Known position flags except sequence. Optional native measurements keep
    // their presence; selecting PDOP does not synthesize it from HDOP.
    // Smart cadence uses GNSS32 fields; seconds clamp only when resolved to ms.
    return settings.valid()&&!(settings.positionFlags&~959u);
  }
  bool setGnssSettings(const GnssSettings &settings) {
    if(!supportedGnssSettings(settings))return false;
    Config candidate=*this;
    if(!settings.encode(candidate.bytes+606,GnssSettings::Size))return false;
    if(!candidate.positionPotential())candidate.revokePositionSharing();
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  unsigned primaryPositionPrecision()const{return bytes[2]>=15?bytes[122]:positionSharing()?PublicPositionPrecision:0;}
  void setPositionSharing(bool enabled){
    if(!enabled){revokePositionSharing();seal();return;}
    bytes[102]=enabled&&positionPotential();
    if(bytes[102]&&!bytes[122])bytes[122]=PublicPositionPrecision;
    seal();
  }
  bool setPrimaryPositionPrecision(unsigned precision) {
    if(precision>32||(precision&&!positionPotential()))return false;
    bytes[122]=uint8_t(precision);
    if(precision)bytes[102]=1;
    else {
      bool other=false;
      for(unsigned i=1;i<8;++i){ChannelRecord c;if(channelRecord(i,c)&&c.role()==2&&c.precision())other=true;}
      if(!other)bytes[102]=0;
    }
    seal();return true;
  }
  bool isPositionEventChannel(unsigned index) const {
#if defined(USERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL) && \
    USERPREFS_BLOCK_POSITION_ON_EVENT_CHANNEL && defined(USERPREFS_CHANNEL_0_PSK)
    static const uint8_t configuredEventPsk[] = USERPREFS_CHANNEL_0_PSK;
    static_assert(sizeof(configuredEventPsk) == 16 || sizeof(configuredEventPsk) == 32,
                  "USERPREFS_CHANNEL_0_PSK must be an AES-128 or AES-256 key");
    PrimaryChannel::Selection selection{};
    const bool matches = channelSelection(index, selection) &&
        selection.length == sizeof(configuredEventPsk) &&
        !memcmp(selection.bytes, configuredEventPsk, sizeof(configuredEventPsk));
    PrimaryChannel::clear(selection);
    return matches;
#else
    (void)index;
    return false;
#endif
  }
  // Forwarding uses channel policy even when the relay has no local location.
  unsigned relayPositionPrecision(unsigned index) const;
  unsigned positionPrecision(unsigned index) const;
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
  uint32_t regionCode()const{return bytes[4];}
  const char *regionName()const {
    const auto *region=radio::RegionPolicy::lookup(regionCode());
    return region?region->name:"";
  }
  static bool supportedRegionCode(uint32_t code,bool explicitlyTxOff=false) {
    const auto *region=radio::RegionPolicy::lookup(code);
    return region&&(code==0||radio::RegionPolicy::tunable(*region,explicitlyTxOff));
  }
  uint32_t channelNumber()const{return bytes[2]>=14?get32(bytes+600):uint16_t(bytes[105])|(uint16_t(bytes[106])<<8);}
  bool usePreset()const{return bytes[2]<14||!(bytes[574]&1);}
  bool powerSpecified()const{return bytes[2]>=14&&(bytes[574]&2);}
  static const char *radioPresetName(uint32_t preset) {
    if(const char *stockName=PrimaryChannel::presetName(preset))return stockName;
    switch(preset) {
    case 10:return "LiteFast";case 11:return "LiteSlow";
    case 12:return "NarrowFast";case 13:return "NarrowSlow";
    case 14:return "TinyFast";case 15:return "TinySlow";
    default:return nullptr;
    }
  }
  const char *modemName()const{return usePreset()?radioPresetName(modemPreset()):"Custom";}
  static float getFloat32(const uint8_t *p){const auto bits=get32(p);float value;static_assert(sizeof(value)==sizeof(bits));memcpy(&value,&bits,sizeof(value));return value;}
  static void putFloat32(uint8_t *p,float value){uint32_t bits;memcpy(&bits,&value,sizeof(bits));put32(p,bits);}
  RadioSettings::Request radioRequest()const {
    RadioSettings::Request r;r.regionCode=regionCode();r.usePreset=usePreset();r.preset=modemPreset();r.channel=channelNumber();r.powerSpecified=powerSpecified();
    if(bytes[2]>=14){r.bandwidth=get32(bytes+576);r.spreadingFactor=get32(bytes+580);r.codingRate=get32(bytes+584);r.power=getSigned32(bytes+588);r.overrideMHz=getFloat32(bytes+592);r.offsetMHz=getFloat32(bytes+596);}return r;
  }
  bool resolveRadio(LoRaPresets::Profile &p,USChannelSelection &s)const {
    const auto request=radioRequest();
    const char *display=request.usePreset?radioPresetName(request.preset):"Custom";
    if(!request.regionCode)
      return RadioSettings::validateUnprovisioned(request,p,s,effectivePrimaryName());
    LoRaPresets::Profile candidateProfile{};USChannelSelection candidateSelection{};
    if(!RadioSettings::resolve(request,candidateProfile,candidateSelection,effectivePrimaryName(),display,txIntent()==TxIntent::Disabled)||
       candidateSelection.regionCode!=request.regionCode)return false;
    p=candidateProfile;s=candidateSelection;return true;
  }
  int effectivePower(bool t096)const{return RadioSettings::effectivePower(radioRequest(),t096);}
  bool setRadio(const RadioSettings::Request &input) {
    auto r=input;
    LoRaPresets::Profile p{};USChannelSelection s{};
    const char *name=bytes[109]?primaryName():(r.usePreset?radioPresetName(r.preset):"Custom");
    uint32_t selectedRegion=0;
    if(!r.regionCode) {
      // Preserve the legacy UNSET configuration editor semantics for dormant
      // records. The compatibility calculation supplies structural geometry
      // only; byte4 remains zero and provisioned() stays false until an
      // explicit supported region is selected.
      if(!RadioSettings::validateUnprovisioned(r,p,s,name))return false;
    } else {
      if(!RadioSettings::normalizeRegionPreset(r.regionCode,r.usePreset,r.preset,selectedRegion,txIntent()==TxIntent::Disabled))return false;
      r.regionCode=selectedRegion;
      const char *display=r.usePreset?radioPresetName(r.preset):"Custom";
      if(!RadioSettings::resolve(r,p,s,name,display,txIntent()==TxIntent::Disabled)) {
        // Retain the established US editor behavior for an oversized channel:
        // the compatibility resolver retries the computed name-hash slot.
        // Regional resolution remains strict until its stock clamp behavior
        // is explicitly represented by that region's policy.
        if(selectedRegion!=1||!RadioSettings::resolve(r,p,s,name))return false;
      }
    }
    const char *defaultName=r.usePreset?radioPresetName(r.preset):"Custom";
    if(!r.overrideMHz&&r.channel>s.slotCount&&(r.usePreset||strcmp(name,defaultName)))r.channel=uint32_t(s.slot)+1;
    Config candidate=*this;candidate.bytes[2]=Version;candidate.bytes[4]=uint8_t(selectedRegion);candidate.bytes[104]=uint8_t(r.preset);candidate.bytes[105]=uint8_t(r.channel);candidate.bytes[106]=uint8_t(r.channel>>8);
    candidate.bytes[574]=uint8_t((r.usePreset?0:1)|(r.powerSpecified?2:0));candidate.bytes[575]=0;
    put32(candidate.bytes+576,r.bandwidth);put32(candidate.bytes+580,r.spreadingFactor);put32(candidate.bytes+584,r.codingRate);
    putSigned32(candidate.bytes+588,r.powerSpecified?ManualLoRa::regionalPower(r.power,30,false):0);
    putFloat32(candidate.bytes+592,r.overrideMHz);putFloat32(candidate.bytes+596,r.offsetMHz);put32(candidate.bytes+600,r.channel);candidate.bytes[605]=0;
    auto record=candidate.node();if(!record.valid())return false;
    if(candidate.licensedOperation()) {
      PrimaryChannel::Selection key;
      if(!PrimaryChannel::normalizeLicensedPlaintext(candidate.effectivePrimaryName(),key))return false;
      record.setLicensedPlaintext(key.hash);PrimaryChannel::clear(key);candidate.node(record);
      candidate.bytes[604]=0;
    } else if(record.bytes[3]) {
      PrimaryChannel::Selection key;
      if(!PrimaryChannel::normalize(record.bytes+bench::MtNodeRecord::Key,record.bytes[3],key,candidate.effectivePrimaryName()))return false;
      record.setKey(key.hash,key.bytes,key.length);PrimaryChannel::clear(key);candidate.node(record);
    } else {
      // Match stock Channels::fixupChannel: a primary mode without a resolved
      // PSK is normalized off, never persisted as an unusable AEAD channel.
      candidate.bytes[604]=0;
    }
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  // Whole-record licensed-mode transition. It never creates a key or infers
  // licensed intent from an empty key; the V2 marker and persisted intent are
  // committed together with the channel and radio policy.
  bool enableLicensedOperation() {
    if(!valid())return false;
    if(licensedOperation())return licensedChannelsValid();
    if(!channelConfigured()||isAdminChannelName(effectivePrimaryName())||
       !DeviceRolePolicy::storedValid(role(),DeviceRolePolicy::RebroadcastMode::LOCAL_ONLY))return false;
    PrimaryChannel::Selection plaintext;
    if(!PrimaryChannel::normalizeLicensedPlaintext(effectivePrimaryName(),plaintext))return false;
    Config candidate=*this;auto primary=candidate.node();
    primary.setLicensedPlaintext(plaintext.hash);
    primary.write32(bench::MtNodeRecord::Interval,600);primary.seal();
    PrimaryChannel::clear(plaintext);candidate.node(primary);
    candidate.bytes[125]=uint8_t((candidate.bytes[125]|3u)&uint8_t(~0x80u));candidate.bytes[604]=0;
    candidate.bytes[108]=uint8_t(DeviceRolePolicy::RebroadcastMode::LOCAL_ONLY);
    for(unsigned i=1;i<8;++i) {
      ChannelRecord channel;if(!this->channelRecord(i,channel))return false;
      const bool admin=isAdminChannelName(channel.name());
      const unsigned targetRole=admin?0:channel.role();
      ChannelRecord cleared;
      if(!cleared.set(targetRole,channel.name(),nullptr,0,channel.precision(),channel.muted(),
                      false,admin?false:channel.downlink()))return false;
      candidate.storeSecondaryRecord(i,cleared);
    }
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
  }
  unsigned hopLimit()const{return bytes[121];}
  bool setHopLimit(unsigned value){if(!HopPolicy::valid(value))return false;bytes[121]=uint8_t(value);seal();return true;}
  unsigned role()const{return bytes[107];}
  unsigned rebroadcastMode()const{return bytes[108];}
  bool setDeviceRole(uint32_t roleValue,uint32_t mode) {
    if(!DeviceRolePolicy::valid(roleValue,mode))return false;
    Config candidate=*this;candidate.bytes[107]=uint8_t(roleValue);candidate.bytes[108]=uint8_t(mode);candidate.seal();
    if(!candidate.valid())return false;*this=candidate;return true;
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
    const auto record=node();
    return canonicalPrimaryName()&&licensedOperation()==record.licensedPlaintext()&&
      PrimaryChannel::configured(record,effectivePrimaryName());
  }
  static bool isAdminChannelName(const char *name) {
    static constexpr char admin[]="admin";
    if(!name)return false;
    for(size_t i=0;i<sizeof(admin);++i) {
      unsigned char c=static_cast<unsigned char>(name[i]);
      if(c>='A'&&c<='Z')c=static_cast<unsigned char>(c-'A'+'a');
      if(c!=static_cast<unsigned char>(admin[i]))return false;
    }
    return true;
  }
  bool licensedChannelsValid()const {
    if(!licensedOperation())return true;
    if(adminChannelEnabled()||primaryAead()||!channelConfigured()||isAdminChannelName(effectivePrimaryName()))return false;
    for(unsigned i=1;i<8;++i) {
      ChannelRecord channel;if(!channelRecord(i,channel))return false;
      if(channel.keySize()||
         (isAdminChannelName(channel.name())&&channel.role()!=0))return false;
    }
    return true;
  }
  bool setPrimaryName(const char *name) {
    if(!PrimaryChannel::validName(name))return false;
    Config candidate=*this;memset(candidate.bytes+109,0,12);for(unsigned i=0;name[i];++i)candidate.bytes[109+i]=uint8_t(name[i]);
    if(!candidate.setRadio(candidate.radioRequest()))return false;
    *this=candidate;return true;
  }
  // One atomic swap. Materialize inherited keys against the old primary
  // before moving it, so the other six logical channels keep their keys.
  bool promoteChannel(unsigned selected) {
    uint8_t order[8];if(!valid()||!ChannelOrder::permutation(selected,order))return false;
    if(!selected)return true;
    ChannelRecord records[8];
    for(unsigned i=0;i<8;++i) {
      if(!channelRecord(i,records[i]))return false;
      if(i&&records[i].role()==2&&!records[i].keySize()) {
        PrimaryChannel::Selection key;
        if(!channelSelection(i,key)){PrimaryChannel::clear(key);return false;}
        const bool ok=records[i].set(2,records[i].name(),key.bytes,key.length,records[i].precision(),records[i].muted(),false,records[i].downlink(),records[i].aead());
        PrimaryChannel::clear(key);if(!ok)return false;
      }
    }
    if(records[selected].role()!=2)return false;
    Config candidate=*this;auto n=candidate.node();PrimaryChannel::Selection key;
    const auto &primary=records[order[0]];
    const char *name=primary.name()[0]?primary.name():modemName();
    if(candidate.licensedOperation()) {
      if(isAdminChannelName(name)||!PrimaryChannel::normalizeLicensedPlaintext(name,key))return false;
      n.setLicensedPlaintext(key.hash);
    } else {
      if(!PrimaryChannel::normalize(primary.key(),primary.keySize(),key,name))return false;
      // MtNodeRecord stores the base (non-AEAD) stock hash; AEAD is applied
      // only to the selected routing hash in PrimaryChannel::Selection.
      n.setKey(key.hash,key.bytes,key.length);
    }
    PrimaryChannel::clear(key);candidate.node(n);
    memset(candidate.bytes+109,0,12);memcpy(candidate.bytes+109,primary.name(),strlen(primary.name()));
    candidate.bytes[101]=primary.muted();candidate.bytes[122]=uint8_t(primary.precision());
    candidate.bytes[123]=primary.downlink()?1:0;
    candidate.bytes[604]=primary.aead()?1:0;
    for(unsigned i=1;i<8;++i) {
      const auto &old=records[order[i]];ChannelRecord moved;
      if(!moved.set(old.role()?2:0,old.name(),old.key(),old.keySize(),old.precision(),old.muted(),false,old.downlink(),old.aead()))return false;
      candidate.storeSecondaryRecord(i,moved);
    }
    // An auto channel follows the promoted primary name, as a normal primary
    // channel edit does. Explicit channel/frequency settings are preserved.
    if(!candidate.setRadio(candidate.radioRequest()))return false;
    candidate.seal();if(!candidate.valid())return false;*this=candidate;return true;
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
  void revokePositionSharing() {
    bytes[102]=0;bytes[122]=0;
    for(unsigned i=1;i<8;++i){ChannelRecord c;if(channelRecord(i,c)&&c.precision()){c.bytes[3]=0;c.seal();storeSecondaryRecord(i,c);}}
  }
  bool setFixedPosition(int32_t latitude,int32_t longitude,bool hasAltitude,int32_t altitudeMetres) {
    if(latitude<-900000000||latitude>900000000||longitude<-1800000000||longitude>1800000000)return false;
    revokePositionSharing(); // New coordinates require fresh consent per channel.
    bytes[84]=PositionValid|(hasAltitude?AltitudeValid:0);
    putSigned32(bytes+85,latitude);putSigned32(bytes+89,longitude);
    putSigned32(bytes+93,hasAltitude?altitudeMetres:0);seal();return true;
  }
  void clearFixedPosition(){revokePositionSharing();memset(bytes+84,0,13);seal();}
  bool validBody(uint8_t version) const {
    if(version<4||version>Version)return false;
    const size_t recordSize=version>=16?Size:version>=14?RadioSize:version>=13?BankSize:version>=11?PrimarySize:FormerSize;
    if(bytes[0]!='M'||bytes[1]!='T'||bytes[2]!=version||bytes[3]||
       (version<18?bytes[4]>1:!supportedRegionCode(bytes[4],version>=24&&txIntent()==TxIntent::Disabled))||(version<19?bytes[5]!=0:bytes[5]>1)||
       !node().valid()||(version<20&&node().bytes[2]>=bench::MtNodeRecord::CurrentVersion)||
       (version<8&&node().interval()>86400)||!validDisplaySeconds(displaySeconds())||bytes[82]>1||bytes[83]>2)return false;
#if defined(CONFIG_MT_LOCAL_CLOCK)
    if(version>=25&&!validClock())return false;
#endif
    if(version>=24){
      const unsigned count=ignoredSenderCount();if(count>IgnoreLimit)return false;
      for(unsigned i=count;i<IgnoreLimit;++i)if(ignoredSender(i))return false;
    }
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
      if(bytes[102]>1||(bytes[102]&&!positionPotential()))return false;
      if(bytes[103]>1||(version<9&&bytes[103]))return false;
      if(version<10){for(unsigned i=104;i<recordSize-2;++i)if(bytes[i])return false;}
      else {
        LoRaPresets::Profile p{};USChannelSelection s{};
        if(version>=11) {
          if(!(version>=19?DeviceRolePolicy::storedValid(role(),rebroadcastMode()):
                            (role()<=2&&DeviceRolePolicy::valid(role(),rebroadcastMode())))||!canonicalPrimaryName()||
             !(version>=18?(regionCode()?resolveRadio(p,s):
                                    RadioSettings::validateUnprovisioned(radioRequest(),p,s,effectivePrimaryName())):
               version>=14?(regionCode()?RadioSettings::resolve(radioRequest(),p,s,effectivePrimaryName()):
                 RadioSettings::validateUnprovisioned(radioRequest(),p,s,effectivePrimaryName())):
               RadioSettings::resolve(modemPreset(),channelNumber(),p,s,effectivePrimaryName())))return false;
          if(version>=12&&!HopPolicy::valid(hopLimit()))return false;
          if(version>=14&&((bytes[574]&uint8_t(~3u))||bytes[575]||(version<23&&bytes[604])||(version>=23&&bytes[604]>1)||bytes[605]||(!powerSpecified()&&get32(bytes+588))||
             uint16_t(channelNumber())!=(uint16_t(bytes[105])|(uint16_t(bytes[106])<<8))))return false;
          if(version>=15&&bytes[122]>32)return false;
          if(version>=17&&bytes[123]>1)return false;
          if(version>=17&&bytes[124]>1)return false;
          if(version==20&&(bytes[125]&0xf0u))return false;
          if(version==21&&(bytes[125]&0xc0u))return false;
          if(version>=20&&((bytes[125]>>2)&3u)==3u)return false;
          if(version>=20&&(licensedOperation()!=node().licensedPlaintext()))return false;
          if(version>=20&&!licensedOperation()&&overrideDutyCycle())return false;
          if(version>=22&&licensedOperation()&&adminChannelEnabled())return false;
          if(version>=23&&primaryAead()&&(licensedOperation()||!channelConfigured()))return false;
      if(version>=17&&bytes[123]&&!channelConfigured())return false;
          for(unsigned i=version>=20?126:version>=17?125:version>=15?123:version>=12?122:121;i<(version>=13?126:recordSize-2);++i)if(bytes[i])return false;
          if(version>=20&&!licensedChannelsValid())return false;
          if(version>=13)for(unsigned i=1;i<8;++i){ChannelRecord c;if(!channelRecord(i,c)||(c.role()!=0&&c.role()!=2)||c.uplink()||(version<17&&c.downlink())||(c.role()==0&&(c.downlink()||c.aead()))||(version<15&&c.precision())||(version<23&&c.aead()))return false;PrimaryChannel::Selection key;const bool usable=c.role()==0||channelSelection(i,key);PrimaryChannel::clear(key);if(!usable)return false;}
        } else {
          if(!RadioSettings::resolve(modemPreset(),channelNumber(),p,s,"LongFast"))return false;
          for(unsigned i=107;i<recordSize-2;++i)if(bytes[i])return false;
        }
      }
    }
    if(version>=16){
      GnssSettings settings;if(!GnssSettings::decode(bytes+606,GnssSettings::Size,settings))return false;
      // Validate legacy layouts with their original capability boundary before
      // migration, so previously invalid bits cannot acquire new meaning.
      if(version<19){if((settings.positionFlags&~703u)||((settings.positionFlags&8u)&&!(settings.positionFlags&16u)))return false;}
      else if(!supportedGnssSettings(settings))return false;
    }
    return (uint16_t(bytes[recordSize-2])|(uint16_t(bytes[recordSize-1])<<8))==checksum(bytes,recordSize-2);
  }
  bool valid() const {
#if defined(CONFIG_MT_LOCAL_CLOCK)
    if(bytes[2]==24)return validBody(24);
#endif
    return validBody(Version);
  }
  bool provisioned() const {return valid()&&regionCode()!=0&&channelConfigured();}
  bool same(const Config &other) const {return !memcmp(bytes,other.bytes,Size);}
  // Exact persisted semantics outside fixed-position bytes 84-96 and GNSS bytes 606-637. CRC bytes are derived and
  // therefore excluded. Callers validate both records before using this to
  // select the narrow live position publication path.
  bool sameExceptPosition(const Config &other) const {
    return !memcmp(bytes,other.bytes,84)&&!memcmp(bytes+97,other.bytes+97,606-97);
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
      if(p[2]<16||p[2]>Version||!candidate.validBody(p[2]))return false;
    }
    else if(n==RadioSize){
      memcpy(candidate.bytes,p,RadioSize);
      if((p[2]!=14&&p[2]!=15)||!candidate.validBody(p[2]))return false;
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
    if(n!=Size&&n!=RadioSize&&n!=BankSize&&n!=PrimarySize) {
      candidate.bytes[2]=14;memset(candidate.bytes+107,0,126-107);candidate.bytes[107]=1;candidate.bytes[121]=HopPolicy::Initial;
      // Old nondefault presets still used LongFast for both crypto and slot.
      if(candidate.modemPreset())memcpy(candidate.bytes+109,"LongFast",9);
      candidate.resetSecondaryChannels();candidate.seal();
    }
    if(n==PrimarySize){candidate.bytes[2]=14;if(p[2]==11)candidate.bytes[121]=HopPolicy::Initial;candidate.resetSecondaryChannels();candidate.seal();}
    if(n!=Size&&n!=RadioSize){const uint32_t channel=uint16_t(candidate.bytes[105])|(uint16_t(candidate.bytes[106])<<8);candidate.bytes[2]=14;memset(candidate.bytes+574,0,Size-574);put32(candidate.bytes+580,11);put32(candidate.bytes+584,5);put32(candidate.bytes+600,channel);candidate.seal();}
    if(p[2]<15)candidate.bytes[122]=candidate.positionSharing()?PublicPositionPrecision:0;
    // V16 already owns the GNSS body. The V17 transition adds MQTT consent
    // bytes only, so preserve its settings while older formats default them.
    if(p[2]<16)memset(candidate.bytes+606,0,GnssSettings::Size);
    if(p[2]<Version){
      // Preserve each version's defined policy bits. Only V21 predates the
      // debug-log/Admin bits; V22 already owns all eight byte125 controls.
      if(p[2]<22)candidate.bytes[125]=uint8_t(candidate.bytes[125]&(p[2]>=21?0x3fu:0x0fu));
      candidate.bytes[2]=Version;candidate.seal();
    }
    if(!candidate.valid())return false;
    out=candidate;return true;
  }
};
static_assert(sizeof(Config)==Config::Size);
}
