// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Config.h"
#include "../ModuleSettings.h"
#include "../SettingsJournal.h"
#include "../modules/CompletionSettings.h"
#include "../notifications/NotificationSettings.h"
#include "../network/MqttGatewaySettings.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::admin {

// Fixed typed projection of the existing RouterRoleDefaults, channel-consent,
// and core MQTT journals.
// The byte representation is canonical because each member is an existing
// fixed-size record whose valid() checks include its own version/seal.
struct RemoteDeviceBundle {
  Config core{};
  ModuleSettings modules{};
  ::mt::modules::CompletionSettings completion{};
  NotificationSettings notifications{};
  ::mt::network::MqttGatewaySettings mqtt{};

  static constexpr size_t CoreBytes=Config::Size;
  static constexpr size_t ModuleBytes=ModuleSettings::Size;
  static constexpr size_t CompletionBytes=::mt::modules::CompletionSettings::Size;
  static constexpr size_t NotificationBytes=NotificationSettings::Size;
  static constexpr size_t MqttBytes=::mt::network::MqttGatewaySettings::Size;
  static constexpr size_t Bytes=CoreBytes+ModuleBytes+CompletionBytes+NotificationBytes+MqttBytes;
  static constexpr uint8_t SectionCount=5;

  bool valid()const {
    return core.valid()&&modules.valid()&&completion.valid()&&notifications.valid()&&mqtt.valid();
  }
  // Historical pending records authenticate their original 640-byte Config.
  // Read and hash that representation verbatim. Only the typed journal save
  // migrates it to the current format.
  static bool validIntentCore(const Config &c) {
    const uint8_t v=c.bytes[2];
    return v>=22&&v<=Config::Version&&c.validBody(v);
  }
  bool validIntent()const {
    return validIntentCore(core)&&modules.valid()&&completion.valid()&&notifications.valid()&&mqtt.valid();
  }
  const uint8_t *section(uint8_t id,size_t &size)const {
    switch(id) {
      case 0:size=CoreBytes;return core.bytes;
      case 1:size=ModuleBytes;return modules.bytes;
      case 2:size=CompletionBytes;return completion.bytes;
      case 3:size=NotificationBytes;return notifications.bytes;
      case 4:size=MqttBytes;return mqtt.bytes;
      default:size=0;return nullptr;
    }
  }
  static size_t sectionSize(uint8_t id) {
    switch(id) {
      case 0:return CoreBytes;case 1:return ModuleBytes;
      case 2:return CompletionBytes;case 3:return NotificationBytes;
      case 4:return MqttBytes;
      default:return 0;
    }
  }
};

static_assert(sizeof(RemoteDeviceBundle)==RemoteDeviceBundle::Bytes,
              "remote bundle must not contain padding");
static_assert(__is_trivially_copyable(RemoteDeviceBundle),
              "remote bundle must remain a fixed raw record projection");

// Durable intent for the stock role/default projection. It uses only the
// existing SettingsBackend; it has no RF, identity-key, nonce, or erase API.
// Instances contain PSK-bearing candidate and record scratch: place the owner
// instance in the platform's static PSRAM/BSS region where available.
class RemoteDeviceTransaction {
public:
  static constexpr size_t HeaderBytes=52;
  static constexpr size_t DigestBytes=32;
  static constexpr size_t DigestsOffset=HeaderBytes;
  static constexpr size_t BundleOffset=DigestsOffset+RemoteDeviceBundle::SectionCount*DigestBytes;
  static constexpr size_t CrcOffset=BundleOffset+RemoteDeviceBundle::Bytes;
  static constexpr size_t RecordBytes=CrcOffset+4;
  static constexpr uint8_t SectionMask=0x1f;
  static constexpr size_t IdentityDomainBytes=36;

  struct Domain {
    uint32_t node=0;
    uint8_t publicKey[32]{};
  };

  struct Callbacks {
    SettingsBackend *backend=nullptr;
    void *context=nullptr;
    // backendReady() means the existing settings backend is initialized.
    // journalsReady() means all five current typed journals are loaded.
    // storagePauseVerified() proves service owner lock,
    // radio/storage pause, and no competing journal writer for this call.
    bool (*backendReady)(void *)=nullptr;
    bool (*journalsReady)(void *)=nullptr;
    bool (*storagePauseVerified)(void *)=nullptr;
    int (*captureCurrent)(void *,uint8_t section,uint8_t *out,size_t capacity,
                          size_t &length)=nullptr;
    int (*validateCurrent)(void *,uint8_t section,const uint8_t *,size_t)=nullptr;
    // Must route to the pre-existing typed journal save method, which itself
    // performs exact readback. The transaction immediately captures and hashes
    // again before marking that section durable.
    int (*saveExisting)(void *,uint8_t section,const uint8_t *,size_t)=nullptr;
    // Inject the platform's BLAKE2b-256 implementation; no hash is invented
    // here. False is a held crypto/provider failure, never a match.
    bool (*digest256)(void *,const uint8_t *,size_t,uint8_t out[32])=nullptr;
  };

  enum class State:uint8_t {
    Unloaded=0,Absent,Pending,Completed,DomainMismatch,Corrupt,IoError,Held
  };
  struct Outcome {
    State state=State::Unloaded;
    int error=0;
    uint32_t generation=0;
    uint8_t verifiedSectionMask=0;
    bool intentVerified=false;
    bool completeVerified=false;
    bool historicalCompleted=false;
  };

private:
  static constexpr uint8_t Version=1;
  static constexpr uint8_t PendingTag=1;
  static constexpr uint8_t CompleteTag=2;
  static constexpr size_t NodeOffset=16;
  static constexpr size_t KeyOffset=20;
  static constexpr size_t ReservedOffset=6;
  static constexpr size_t LengthOffset=8;
  static constexpr size_t GenerationOffset=12;
  static constexpr size_t MaxSectionBytes=RemoteDeviceBundle::CompletionBytes;

  struct Parsed {
    bool present=false;
    State state=State::Absent;
    uint32_t generation=0;
    Domain domain{};
  };

  State state_=State::Unloaded;
  int8_t selectedSlot_=-1;
  uint32_t generation_=0;
  Domain selectedDomain_{};
  uint8_t selected_[RecordBytes]{};
  uint8_t io_[RecordBytes]{};
  uint8_t current_[MaxSectionBytes]{};
  RemoteDeviceBundle target_{};

  static void wipe(void *memory,size_t size) {
    volatile uint8_t *p=static_cast<volatile uint8_t *>(memory);
    while(size--)*p++=0;
  }
  // Public calls always reload durable intent before using it. Their raw
  // PSK/credential-bearing scratch has no authority between calls.
  struct EndCallWipe {
    RemoteDeviceTransaction &owner;
    ~EndCallWipe(){wipe(owner.selected_,sizeof(owner.selected_));wipe(owner.io_,sizeof(owner.io_));
      wipe(owner.current_,sizeof(owner.current_));wipe(&owner.target_,sizeof(owner.target_));
    }
  };
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static void put32(uint8_t *p,uint32_t value) {
    for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));
  }
  static uint32_t crc32(const uint8_t *p,size_t n) {
    uint32_t c=UINT32_MAX;
    for(size_t i=0;i<n;++i) {
      c^=p[i];
      for(unsigned bit=0;bit<8;++bit)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));
    }
    return ~c;
  }
  static bool domainValid(const Domain &domain) {
    if(domain.node<=1||domain.node>=0xfffffffeu)return false;
    uint8_t any=0;for(uint8_t b:domain.publicKey)any|=b;
    return any!=0;
  }
  static bool sameDomain(const Domain &a,const Domain &b) {
    return a.node==b.node&&!memcmp(a.publicKey,b.publicKey,sizeof(a.publicKey));
  }
  static const char *key(unsigned slot) {
    return slot?"mtrole/intent1":"mtrole/intent0";
  }
  static bool callbacksValid(const Callbacks &cb) {
    return cb.backend&&cb.backendReady&&cb.storagePauseVerified&&cb.captureCurrent&&
      cb.validateCurrent&&cb.saveExisting&&cb.digest256;
  }
  bool gated(const Callbacks &cb,bool requirePause,bool requireJournals=true)const {
    return callbacksValid(cb)&&cb.backendReady(cb.context)&&
      (!requireJournals||(cb.journalsReady&&cb.journalsReady(cb.context)))&&
      (!requirePause||cb.storagePauseVerified(cb.context));
  }
  bool validateRecord(const uint8_t *p,Parsed &parsed) {
    parsed={};
    if(p[0]!='Z'||p[1]!='T'||p[2]!='R'||p[3]!='I'||p[4]!=Version||
       (p[5]!=PendingTag&&p[5]!=CompleteTag)||p[ReservedOffset]||p[ReservedOffset+1]||
       get32(p+LengthOffset)!=RecordBytes||get32(p+CrcOffset)!=crc32(p,CrcOffset))return false;
    const uint32_t gen=get32(p+GenerationOffset);
    Domain domain{};domain.node=get32(p+NodeOffset);memcpy(domain.publicKey,p+KeyOffset,32);
    if(!gen||!domainValid(domain))return false;
    if(p[5]==CompleteTag) {
      for(size_t i=DigestsOffset;i<RecordBytes-4;++i)if(p[i])return false;
    } else {
      memcpy(&target_,p+BundleOffset,sizeof(target_));
      if(!target_.validIntent())return false;
    }
    parsed.present=true;parsed.state=p[5]==PendingTag?State::Pending:State::Completed;
    parsed.generation=gen;parsed.domain=domain;return true;
  }
  void encodeHeader(uint8_t *p,uint8_t tag,uint32_t gen,const Domain &domain) {
    memset(p,0,RecordBytes);p[0]='Z';p[1]='T';p[2]='R';p[3]='I';p[4]=Version;p[5]=tag;
    put32(p+LengthOffset,uint32_t(RecordBytes));put32(p+GenerationOffset,gen);
    put32(p+NodeOffset,domain.node);memcpy(p+KeyOffset,domain.publicKey,32);
  }
  int readIntent(const Callbacks &cb,unsigned slot,Parsed &parsed) {
    const int count=cb.backend->read(key(slot),io_,sizeof(io_));
    if(count==-ENOENT){parsed={};return 0;}
    if(count==-E2BIG){parsed={};parsed.present=true;return -EBADMSG;}
    if(count<0)return count;
    if(count!=int(RecordBytes)||!validateRecord(io_,parsed)) {
      parsed={};parsed.present=true;return -EBADMSG;
    }
    return 0;
  }
  int inspectInternal(const Domain &domain,const Callbacks &cb,Outcome &out) {
    out={};selectedSlot_=-1;generation_=0;selectedDomain_={};state_=State::Unloaded;
    if(!domainValid(domain)||!gated(cb,false,false)) {
      out.state=State::Held;out.error=-EACCES;state_=out.state;return out.error;
    }
    Parsed a{},b{};
    int rc=readIntent(cb,0,a);
    if(rc==-EBADMSG){state_=State::Corrupt;out.state=state_;out.error=-EBADMSG;return out.error;}
    if(rc){state_=State::IoError;out.state=state_;out.error=rc;return rc;}
    if(a.present)memcpy(selected_,io_,RecordBytes);
    rc=readIntent(cb,1,b);
    if(rc==-EBADMSG){state_=State::Corrupt;out.state=state_;out.error=-EBADMSG;return out.error;}
    if(rc){state_=State::IoError;out.state=state_;out.error=rc;return rc;}
    const bool hasA=a.present,hasB=b.present;
    if(!hasA&&!hasB){state_=State::Absent;out.state=state_;return 0;}
    unsigned selected=hasB?1:0;
    if(hasA&&hasB) {
      if(a.generation==b.generation) {
        if(memcmp(selected_,io_,RecordBytes)) {
          state_=State::Corrupt;out.state=state_;out.error=-EINVAL;return out.error;
        }
        selected=0;
      } else selected=b.generation>a.generation?1:0;
    }
    if(selected==1)memcpy(selected_,io_,RecordBytes);
    Parsed &chosen=selected?b:a;
    selectedSlot_=int8_t(selected);generation_=chosen.generation;selectedDomain_=chosen.domain;
    const bool currentDomain=sameDomain(chosen.domain,domain);
    if(!currentDomain&&chosen.state==State::Pending)state_=State::DomainMismatch;
    else state_=chosen.state; // A completed marker is historical, not authority.
    if(state_==State::Pending) {
      memcpy(&target_,selected_+BundleOffset,sizeof(target_));
      if(!target_.validIntent()){state_=State::Corrupt;}
    } else wipe(&target_,sizeof(target_));
    out.state=state_;out.generation=generation_;
    out.intentVerified=state_==State::Pending||(state_==State::Completed&&currentDomain);
    out.completeVerified=state_==State::Completed;
    out.historicalCompleted=state_==State::Completed&&!currentDomain;
    if(state_==State::DomainMismatch){out.error=-EXDEV;return out.error;}
    if(state_==State::Corrupt){out.error=-EBADMSG;return out.error;}
    return 0;
  }
  int captureAndDigest(const Callbacks &cb,uint8_t section,uint8_t digest[32]) {
    static_assert(Config::Version==24 || Config::Version==25,"re-audit legacy Config projection after a format bump");
    const size_t expected=RemoteDeviceBundle::sectionSize(section);size_t length=0;
    if(!expected||expected>sizeof(current_))return -EINVAL;
    memset(current_,0,sizeof(current_));
    const int rc=cb.captureCurrent(cb.context,section,current_,expected,length);
    if(rc)return rc;
    if(length!=expected||cb.validateCurrent(cb.context,section,current_,length))return -EBADMSG;
    const uint8_t intentVersion=target_.core.bytes[2];
    if(section==0&&intentVersion!=current_[2]) {
      // The current journal validated the entire V24 record, including its
      // unused sender slots. A nonempty V24 filter has no V22/V23 equivalent.
      if(current_[2]<24||current_[2]>Config::Version||(intentVersion<24&&current_[Config::IgnoreCountOffset]))return -EBADMSG;
#if defined(CONFIG_MT_LOCAL_CLOCK)
      // An open legacy intent authenticates its original wire bytes. Reject
      // a non-default V25 clock rather than deleting it from the digest.
      if(intentVersion<25)for(unsigned i=0;i<Config::ClockBytes;++i)if(current_[Config::clockOffset(i)])return -EBADMSG;
#endif
      if(intentVersion==22) {
        if(current_[604])return -EBADMSG;
        for(unsigned i=1;i<8;++i)
          if(current_[Config::ChannelTableOffset+(i-1)*ChannelRecord::Size+2]&8u)return -EBADMSG;
      }
      current_[2]=intentVersion;
      const uint16_t crc=Config::checksum(current_,Config::Size-2);
      current_[Config::Size-2]=uint8_t(crc);current_[Config::Size-1]=uint8_t(crc>>8);
    }
    if(!cb.digest256(cb.context,current_,length,digest))return -EIO;
    return 0;
  }
  bool validMqttDelta(const uint8_t *current,size_t length)const {
    using Mqtt=::mt::network::MqttGatewaySettings;
    if(!current||length!=RemoteDeviceBundle::MqttBytes)return false;
    constexpr uint8_t ConfigOkBit=0x80u;
    bool policyChanged=false;
    for(size_t i=0;i<Mqtt::ConsentBase;++i) {
      if(i==Mqtt::Flags) {
        if((current[i]&uint8_t(~ConfigOkBit))!=
           (target_.mqtt.bytes[i]&uint8_t(~ConfigOkBit)))return false;
        policyChanged|=((current[i]^target_.mqtt.bytes[i])&ConfigOkBit)!=0;
      } else if(i==Mqtt::RootLength) {
        policyChanged|=current[i]!=target_.mqtt.bytes[i];
      } else if(i>=Mqtt::Root&&i<Mqtt::Root+32) {
        policyChanged|=current[i]!=target_.mqtt.bytes[i];
      } else if(current[i]!=target_.mqtt.bytes[i])return false;
    }
    unsigned changed=0;
    for(unsigned slot=0;slot<Mqtt::ChannelSlots;++slot) {
      const size_t offset=Mqtt::ConsentBase+size_t(slot)*Mqtt::ChannelConsentSize;
      const uint8_t *consent=target_.mqtt.bytes+offset;
      if(!memcmp(current+offset,consent,Mqtt::ChannelConsentSize))continue;
      if(++changed>1)return false;
      if(policyChanged)return false;
      if(consent[0]) {
        if(!target_.mqtt.channelUplinkEnabled(target_.core,slot))return false;
      } else {
        for(size_t i=0;i<Mqtt::ChannelConsentSize;++i)if(consent[i])return false;
      }
    }
    return true;
  }
  int writeAndVerify(const Callbacks &cb,unsigned slot) {
    int rc=cb.backend->write(key(slot),io_,RecordBytes);
    if(rc)return rc;
    const int count=cb.backend->read(key(slot),selected_,RecordBytes);
    if(count!=int(RecordBytes)||memcmp(io_,selected_,RecordBytes))return count<0?count:-EIO;
    return 0;
  }
  int writeComplete(const Domain &domain,const Callbacks &cb,Outcome &out) {
    if(generation_==UINT32_MAX)return -EOVERFLOW;
    const uint32_t next=generation_+1;
    encodeHeader(io_,CompleteTag,next,domain);
    put32(io_+CrcOffset,crc32(io_,CrcOffset));
    const unsigned slot=selectedSlot_==0?1:0;
    const int rc=writeAndVerify(cb,slot);
    if(rc)return rc;
    Parsed parsed{};
    if(!validateRecord(selected_,parsed)||parsed.state!=State::Completed||
       parsed.generation!=next||!sameDomain(parsed.domain,domain))return -EIO;
    selectedSlot_=int8_t(slot);generation_=next;selectedDomain_=domain;state_=State::Completed;
    wipe(&target_,sizeof(target_));
    out.state=State::Completed;out.generation=generation_;out.intentVerified=true;
    out.completeVerified=true;out.verifiedSectionMask=SectionMask;out.error=0;return 0;
  }
  int rollForward(const Domain &domain,const Callbacks &cb,Outcome &out) {
    if(!gated(cb,true)){state_=State::Held;out.state=state_;out.error=-EBUSY;return out.error;}
    if(state_!=State::Pending||!sameDomain(selectedDomain_,domain)) {
      out.state=state_;out.generation=generation_;out.intentVerified=state_==State::Pending;
      out.completeVerified=state_==State::Completed;out.error=-EINVAL;return out.error;
    }
    const uint32_t intentGeneration=generation_;
    out={};out.state=State::Pending;out.generation=generation_;out.intentVerified=true;
    for(uint8_t section=0;section<RemoteDeviceBundle::SectionCount;++section) {
      if(!gated(cb,true)){state_=State::Held;out.state=state_;out.error=-EBUSY;return out.error;}
      size_t targetLength=0;
      const uint8_t *target=target_.section(section,targetLength);
      uint8_t currentDigest[32]{},targetDigest[32]{};
      const int captured=captureAndDigest(cb,section,currentDigest);
      if(captured){wipe(currentDigest,sizeof(currentDigest));wipe(targetDigest,sizeof(targetDigest));state_=State::Held;out.state=state_;out.error=captured;return captured;}
      if(!cb.digest256(cb.context,target,targetLength,targetDigest)) {
        wipe(currentDigest,sizeof(currentDigest));wipe(targetDigest,sizeof(targetDigest));state_=State::Held;out.state=state_;out.error=-EIO;return out.error;
      }
      const uint8_t *oldDigest=selected_+DigestsOffset+size_t(section)*DigestBytes;
      const bool alreadyCandidate=!memcmp(current_,target,targetLength)&&!memcmp(currentDigest,targetDigest,32);
      const bool matchesBase=!memcmp(currentDigest,oldDigest,32);
      if(!alreadyCandidate&&!matchesBase) {
        wipe(currentDigest,sizeof(currentDigest));wipe(targetDigest,sizeof(targetDigest));
        state_=State::Held;out.state=state_;out.error=-ESTALE;return out.error;
      }
      if(!alreadyCandidate) {
        const int saved=cb.saveExisting(cb.context,section,target,targetLength);
        if(saved) {
          wipe(currentDigest,sizeof(currentDigest));wipe(targetDigest,sizeof(targetDigest));
          state_=State::Held;out.state=state_;out.error=saved;return saved;
        }
        const int verified=captureAndDigest(cb,section,currentDigest);
        if(verified||memcmp(current_,target,targetLength)||memcmp(currentDigest,targetDigest,32)) {
          wipe(currentDigest,sizeof(currentDigest));wipe(targetDigest,sizeof(targetDigest));
          state_=State::Held;out.state=state_;out.error=verified?verified:-EIO;return out.error;
        }
      }
      out.verifiedSectionMask|=uint8_t(1u<<section);
      wipe(currentDigest,sizeof(currentDigest));wipe(targetDigest,sizeof(targetDigest));
      wipe(current_,sizeof(current_));
    }
    if(generation_!=intentGeneration){state_=State::Held;out.state=state_;out.error=-ESTALE;return out.error;}
    if(!gated(cb,true)){state_=State::Held;out.state=state_;out.error=-EBUSY;return out.error;}
    const int complete=writeComplete(domain,cb,out);
    if(complete){state_=State::Held;out.state=state_;out.generation=generation_;
      out.intentVerified=true;out.verifiedSectionMask=SectionMask;out.completeVerified=false;out.error=complete;return complete;}
    return 0;
  }

public:
  static_assert(RecordBytes==2588,"remote transaction intent layout changed");
  static_assert(BundleOffset+RemoteDeviceBundle::Bytes==CrcOffset,
                "remote candidate bytes must end at CRC");
  static_assert(NodeOffset+IdentityDomainBytes==HeaderBytes,
                "intent domain layout changed");
  static constexpr size_t intentBytes() {return RecordBytes;}

  RemoteDeviceTransaction()=default;
  RemoteDeviceTransaction(const RemoteDeviceTransaction &)=delete;
  RemoteDeviceTransaction &operator=(const RemoteDeviceTransaction &)=delete;
  ~RemoteDeviceTransaction(){
    wipe(selected_,sizeof(selected_));wipe(io_,sizeof(io_));wipe(current_,sizeof(current_));
    wipe(&target_,sizeof(target_));wipe(&selectedDomain_,sizeof(selectedDomain_));
  }

  State state()const{return state_;}

  // Read-only boot inspection. Call when SETTINGS_NVS is ready, before loading
  // or applying all projected journals. Corrupt/IO/pending-domain errors
  // are explicit states and are never converted to Absent.
  int inspect(const Domain &domain,const Callbacks &cb,Outcome &out) {
    EndCallWipe wipeCall{*this};
    return inspectInternal(domain,cb,out);
  }

  // Persist a complete roll-forward intent before invoking any section save.
  // The caller holds the authoritative Service lock/storage pause. This
  // function returns a partial mask if a later section or completion write
  // fails; only completeVerified permits success ACK/live candidate apply.
  int beginAndCommit(const Domain &domain,const RemoteDeviceBundle &candidate,
      const Callbacks &cb,Outcome &out) {
    EndCallWipe wipeCall{*this};
    out={};
    if(!domainValid(domain)||!candidate.valid()||!gated(cb,true)) {
      out.state=State::Held;out.error=-EINVAL;state_=out.state;return out.error;
    }
    Outcome prior{};const int inspectRc=inspectInternal(domain,cb,prior);
    if(inspectRc){out=prior;return inspectRc;}
    if(state_==State::Pending){out=prior;out.state=State::Held;out.error=-EBUSY;state_=State::Held;return out.error;}
    if(state_!=State::Absent&&state_!=State::Completed) {
      out=prior;out.state=State::Held;out.error=-EACCES;state_=State::Held;return out.error;
    }
    if(generation_>UINT32_MAX-2u){out.state=State::Held;out.error=-EOVERFLOW;state_=out.state;return out.error;}
    target_=candidate;
    const uint32_t pendingGeneration=generation_+1;
    encodeHeader(io_,PendingTag,pendingGeneration,domain);
    for(uint8_t section=0;section<RemoteDeviceBundle::SectionCount;++section) {
      if(!gated(cb,true)){wipe(&target_,sizeof(target_));state_=State::Held;out.state=state_;out.error=-EBUSY;return out.error;}
      uint8_t *oldDigest=io_+DigestsOffset+size_t(section)*DigestBytes;
      const int rc=captureAndDigest(cb,section,oldDigest);
      if(rc){wipe(&target_,sizeof(target_));state_=State::Held;out.state=state_;out.error=rc;return rc;}
      if(section==4&&!validMqttDelta(current_,RemoteDeviceBundle::MqttBytes)) {
        wipe(current_,sizeof(current_));wipe(&target_,sizeof(target_));
        state_=State::Held;out.state=state_;out.error=-EPERM;return out.error;
      }
      wipe(current_,sizeof(current_));
    }
    memcpy(io_+BundleOffset,&target_,sizeof(target_));
    put32(io_+CrcOffset,crc32(io_,CrcOffset));
    const unsigned slot=selectedSlot_<0?0:unsigned(1-selectedSlot_);
    const int written=writeAndVerify(cb,slot);
    if(written) {
      state_=State::Held;out.state=state_;out.error=written;
      // The write may have committed despite an error. Do not start section
      // writes; an explicit inspect/reboot must establish durable state.
      wipe(&target_,sizeof(target_));return written;
    }
    Parsed pending{};
    if(!validateRecord(selected_,pending)||pending.state!=State::Pending||
       pending.generation!=pendingGeneration||!sameDomain(pending.domain,domain)) {
      state_=State::Held;out.state=state_;out.error=-EIO;wipe(&target_,sizeof(target_));return out.error;
    }
    selectedSlot_=int8_t(slot);generation_=pendingGeneration;selectedDomain_=domain;
    memcpy(&target_,selected_+BundleOffset,sizeof(target_));state_=State::Pending;
    out.state=State::Pending;out.generation=generation_;out.intentVerified=true;
    return rollForward(domain,cb,out);
  }

  // Startup roll-forward. The newest verified Pending intent is the only
  // source of target records. Each current section must match its captured old
  // digest or exact target bytes; all other states remain held.
  int resume(const Domain &domain,const Callbacks &cb,Outcome &out) {
    EndCallWipe wipeCall{*this};
    const int rc=inspectInternal(domain,cb,out);
    if(rc)return rc;
    if(state_==State::Absent||state_==State::Completed)return 0;
    if(state_!=State::Pending){out.state=State::Held;out.error=-EACCES;return out.error;}
    return rollForward(domain,cb,out);
  }
};

static_assert(RemoteDeviceTransaction::intentBytes()<4096,
              "each intent value must remain below the current NVS sector bound");

} // namespace mt::admin
