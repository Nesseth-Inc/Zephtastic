// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Discovery-only endpoint for Meshtastic2.8.1 commit8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c.
// User fields1/2/3/5/7/9; Data NODEINFO_APP=4. No PKI key, DM or ACK endpoint.
#include "MtCodec.h"
#include "MtKeyCommand.h"
#include "RelayCore.h"
#include <stdio.h>
namespace bench {
// Field 5 is a protobuf varint. The supported uint8_t model range takes up
// to three bytes including its tag, even with a full-length owner name.
inline void appendNodeInfoHardwareModel(uint8_t *user,size_t &n,uint8_t model) {
  user[n++]=0x28;
  user[n++]=model; // For 128..255 the high bit is the varint continuation flag.
  if(model>=128)user[n++]=1;
}

struct MtNodeRecord {
  static constexpr uint32_t MaxIntervalSeconds=2147483u; // INT32_MAX / 1000
  static constexpr uint8_t CurrentVersion=2;
  uint8_t bytes[72]{};
  enum { Key=5, Interval=37, Name=41, Short=66, Check=70 };
  // Explicit owner-selected default Public/LongFast channel. This well-known
  // AES128 key is public protocol data, not a private network credential.
  // Pinned upstream src/mesh/Channels.h,commit8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c.
  static constexpr uint8_t PublicKey[16]={0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,
    0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01};
  static constexpr uint8_t publicHash() {
    uint8_t hash=0;for(uint8_t byte:PublicKey)hash^=byte;
    for(char byte:"LongFast")hash^=uint8_t(byte);
    return hash;
  }
  MtNodeRecord() {
    bytes[0]='M';bytes[1]='N';bytes[2]=1;
    write32(Interval,10800);memcpy(bytes+Name,"XIAO Dual",10);memcpy(bytes+Short,"DUAL",4);seal();
  }
  ~MtNodeRecord() {wipeKeyBuffer(bytes,sizeof(bytes));}
  void write32(unsigned offset,uint32_t n) {for(unsigned i=0;i<4;++i)bytes[offset+i]=uint8_t(n>>(8*i));}
  // Meshtastic MAX_INTERVAL is an explicit sentinel in seconds on the wire.
  static bool intervalSupported(uint32_t seconds){return seconds<=MaxIntervalSeconds||seconds==INT32_MAX;}
  static uint32_t intervalMs(uint32_t seconds){return seconds>MaxIntervalSeconds?uint32_t(INT32_MAX):seconds*1000u;}
  uint32_t interval() const {return le32(bytes+Interval);}
  static bool text(const char *s,size_t max,bool spaces) {
    const size_t n=strlen(s);if(!n||n>max)return false;
    for(size_t i=0;i<n;++i)if(s[i]<33-(spaces?1:0)||s[i]>126||s[i]=='"'||s[i]=='\\')return false;
    return s[n-1]!=' ';
  }
  uint16_t crc() const {
    uint16_t c=0xffff;
    for(unsigned i=0;i<Check;++i){c^=uint16_t(bytes[i])<<8;for(unsigned j=0;j<8;++j)c=c&0x8000?uint16_t((c<<1)^0x1021):uint16_t(c<<1);}
    return c;
  }
  void seal() {const uint16_t c=crc();bytes[Check]=uint8_t(c);bytes[Check+1]=uint8_t(c>>8);}
  bool valid() const {
    if(bytes[0]!='M'||bytes[1]!='N'||(bytes[2]!=1&&bytes[2]!=CurrentVersion)||
       (bytes[3]!=0&&bytes[3]!=16&&bytes[3]!=32)||
       interval()<60||!intervalSupported(interval())||bytes[Name+24])return false;
    char shortText[5]{};memcpy(shortText,bytes+Short,4);
    if(!text(reinterpret_cast<const char *>(bytes+Name),24,true)||!text(shortText,4,false))return false;
    if(bytes[2]==1&&!bytes[3]&&bytes[4])return false;
    for(unsigned i=bytes[3];i<32;++i)if(bytes[Key+i])return false;
    return (uint16_t(bytes[Check])|uint16_t(bytes[Check+1])<<8)==crc();
  }
  bool setName(const char *s,bool shortName) {
    if(!text(s,shortName?4:24,!shortName))return false;
    memset(bytes+(shortName?Short:Name),0,shortName?4:25);
    memcpy(bytes+(shortName?Short:Name),s,strlen(s));seal();return true;
  }
  bool licensedPlaintext() const {return bytes[2]==CurrentVersion&&bytes[3]==0;}
  void setKey(uint8_t hash,const uint8_t *key,size_t n) {
    bytes[2]=1;
    bytes[3]=uint8_t((key&&(n==16||n==32))?n:0);
    bytes[4]=bytes[3]?hash:0;memset(bytes+Key,0,32);if(bytes[3])memcpy(bytes+Key,key,bytes[3]);seal();
  }
  // Explicit V2 mode. A V1 zero-length record remains an unconfigured node;
  // V2 length zero opts into plaintext and carries the canonical stock hash.
  void setLicensedPlaintext(uint8_t hash) {
    bytes[2]=CurrentVersion;bytes[3]=0;bytes[4]=hash;
    memset(bytes+Key,0,32);seal();
  }
  void setPublic() {setKey(publicHash(),PublicKey,sizeof(PublicKey));}
  bool isPublic() const {return bytes[3]==sizeof(PublicKey)&&bytes[4]==publicHash()&&!memcmp(bytes+Key,PublicKey,sizeof(PublicKey));}
};

class MtNodeInfo {
  MtCodec codec_;
  MtNodeRecord record_;
  Frame pending_;
  uint32_t node_=0,nextAt_=0,lastAt_=0,lastId_=0;
  uint8_t hardwareModel_=81;
  bool storageValid_=true,inFlight_=false,wasEligible_=false,sentBefore_=false;
  static void stringField(uint8_t *p,size_t &n,uint8_t tag,const char *s) {
    const size_t len=strlen(s);p[n++]=uint8_t((tag<<3)|2);p[n++]=uint8_t(len);memcpy(p+n,s,len);n+=len;
  }
public:
  uint32_t queued=0,completed=0,failed=0,expired=0,coalesced=0,cancelled=0,encodeFailed=0;
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  void restrictOrigins(const ChannelOriginPolicy &policy){cancel();codec_.restrictOrigins(policy);}
#endif
  void identity(uint32_t node) {node_=node;}
  void hardwareModel(uint8_t model) {hardwareModel_=model;}
  uint32_t node() const {return node_;}
  const MtNodeRecord &record() const {return record_;}
  bool configured() const {return storageValid_&&codec_.configured()&&node_>1&&node_<0xfffffffeu;}
  bool storageValid() const {return storageValid_;}
  void invalidate() {cancel();storageValid_=false;codec_.clearKey();}
  bool configure(const MtNodeRecord &record,bool aead=false) {
    if(!record.valid())return false;
    cancel();record_=record;storageValid_=true;codec_.clearKey();
    if(record.licensedPlaintext())return !aead&&codec_.setLicensedPlaintext(record.bytes[4]);
    if(!record.bytes[3])return !aead;
    const uint8_t hash=uint8_t(record.bytes[4]^(aead?0xAEu:0u));
    return codec_.setKey(hash,record.bytes+MtNodeRecord::Key,record.bytes[3],aead);
  }
  void cancel() {if(pending_.used||inFlight_)++cancelled;pending_=Frame{};inFlight_=false;wasEligible_=false;}
  bool encode(uint32_t id,Frame &frame) const {
    frame=Frame{};
    if(!configured()||!id||id==lastId_)return false;
    char name[25]{},shortName[5]{},identityText[10]{};
    memcpy(name,record_.bytes+MtNodeRecord::Name,24);memcpy(shortName,record_.bytes+MtNodeRecord::Short,4);
    snprintf(identityText,sizeof(identityText),"!%08x",node_);
    uint8_t user[64]{};size_t n=0;
    stringField(user,n,1,identityText);stringField(user,n,2,name);stringField(user,n,3,shortName);
    appendNodeInfoHardwareModel(user,n,hardwareModel_);
    if(record_.licensedPlaintext()){user[n++]=0x30;user[n++]=1;}
    user[n++]=0x38;user[n++]=0;  // CLIENT;bounded transit,not full ROUTER
    user[n++]=0x48;user[n++]=1;  // has_is_unmessagable=true,is_unmessagable=true
    frame=Frame{};size_t length=0;
    const bool ok=codec_.encodeZeroHopNodeInfo(node_,id,user,n,frame.bytes,sizeof(frame.bytes),length);
    wipeKeyBuffer(user,sizeof(user));frame.len=uint16_t(length);return ok;
  }
  bool request(uint32_t now,uint32_t id,bool eligible) {
    if(!eligible||!configured())return false;
    if(pending_.used||inFlight_){++coalesced;return true;}
    if(sentBefore_&&uint32_t(now-lastAt_)<60000){nextAt_=lastAt_+60000;return false;}
    if(!encode(id,pending_)){++encodeFailed;nextAt_=now+60000;return false;}
    lastId_=id;pending_.used=true;pending_.dueAt=now+250;pending_.expires=now+30000;++queued;return true;
  }
  void poll(uint32_t now,uint32_t id,bool eligible) {
    if(!eligible||!configured()){if(wasEligible_||pending_.used)cancel();return;}
    if(!wasEligible_){wasEligible_=true;nextAt_=now+30000;}
    if(pending_.used&&due(now,pending_.expires)){pending_=Frame{};++expired;nextAt_=now+60000;}
    if(!pending_.used&&!inFlight_&&due(now,nextAt_))(void)request(now,id,true);
  }
  Frame *next(uint32_t now) {return pending_.used&&due(now,pending_.dueAt)?&pending_:nullptr;}
  void started() {pending_.used=false;inFlight_=true;}
  void defer(uint32_t now,uint32_t random) {pending_.dueAt=now+250+random%751;}
  bool retry(const Frame &frame,uint32_t now) {
    inFlight_=false;
    if(frame.attempts>=3||due(now,frame.expires)){++failed;pending_=Frame{};nextAt_=now+60000;return false;}
    pending_=frame;pending_.used=true;pending_.dueAt=now+500;return true;
  }
  void finish(uint32_t now,bool success) {
    inFlight_=false;pending_=Frame{};
    if(success){++completed;sentBefore_=true;lastAt_=now;nextAt_=now+MtNodeRecord::intervalMs(record_.interval());}
    else{++failed;nextAt_=now+60000;}
  }
  uint32_t lastId() const {return lastId_;}
  bool pending() const {return pending_.used||inFlight_;}
};
} // namespace bench
