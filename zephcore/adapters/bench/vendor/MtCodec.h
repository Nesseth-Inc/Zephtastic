// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Bounded relay metadata adapter for Meshtastic firmware 2.8.1, commit
// 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c. Data/Routing wire types and
// CryptoEngine nonce layout are pinned to that source. MtSignature separately
// verifies public signatures. No endpoint PKI keys, ACK proof verification or
// message delivery are provided.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "MtModulePorts.h"
#include "MtChannelAead.h"
#include "../../../../mt_only/src/modules/audio/AudioWire.h"

namespace bench {
struct RouteMetadata {
  // AES-CTR + a valid protobuf is NOT cryptographic authentication.
  bool channelDecoded=false, decodeRejected=false, backendFailed=false;
  // Local multi-key decoder outcome; never imported from wire metadata.
  bool channelAmbiguous=false;
  uint32_t portnum=0, request_id=0, reply_id=0;
  bool routingPresent=false, routingErrorPresent=false;
  uint32_t routingError=0;
  bool nodeInfoValid=false, nodeInfoLicensed=false, traceRouteValid=false;
  uint8_t ackProofLength=0, signatureLength=0;
  // Original signed envelope, retained before Router's want_response merge.
  // Public signatures cover these values and the exact payload bytes, not the
  // protobuf serialization, channel hash, or mutable relay/hop header.
  uint32_t emoji=0;
  uint8_t bitfield=0, payloadLength=0, publicKeyLength=0;
  bool hasBitfield=false, wantResponse=false;
  uint8_t payload[233]{}, signature[64]{}, publicKey[32]{};
  bool signatureVerified=false, signatureQuarantined=false, signatureRejected=false;
  // Plain Data wire size, including any signature/unknown fields. This is not
  // nanopb's canonical re-encoding size; using it gives conservative airtime.
  uint16_t encodedDataSize=0;
  // Local Service decoder context only; never imported from protobuf/RF.
  // Zero preserves standalone single-key codec callers. Configuration changes
  // invalidate every routing association before these indexes may be reused.
  uint8_t channelContext=0;
  bool mqttIngress=false; // Transport owner metadata. Never decoded from a packet.
  // Local-only role policy computed by Service after admission gates. These
  // values are never decoded from packet metadata or serialized on the wire.
  bool clientBaseRole=false,clientBaseFavoriteTraffic=false,preserveHopLimit=false;
  bool routerLateRole=false;
};

using MtAesCtr = bool (*)(void *context,const uint8_t *key,size_t keyLen,
                         const uint8_t nonce[16],uint8_t *bytes,size_t len);
// Runtime implementation uses ZephCore's existing PSA AES-ECB backend. The
// macOS host backend uses CommonCrypto; neither implements AES itself.
bool mtAesCtr(void *,const uint8_t *,size_t,const uint8_t[16],uint8_t *,size_t);

#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
struct ChannelOriginPolicy {
  virtual bool maySealChannel(const uint8_t *key,size_t length,bool plaintext)const=0;
};
#endif

class MtCodec {
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  const ChannelOriginPolicy *originPolicy_=nullptr;
#endif
  uint8_t key_[32]{};
  uint8_t keyLen_=0, hash_=0;
  bool licensedPlaintext_=false,aead_=false,signPublic_=false;
  MtAesCtr crypt_=nullptr;
  void *context_=nullptr;
  static void wipe(void *ptr,size_t n) {
    volatile uint8_t *p=static_cast<volatile uint8_t *>(ptr);
    while(n--) *p++=0;
  }
  bool transform(uint8_t *bytes,size_t len,const uint8_t nonceBytes[16]) const {
    if(!bytes||len>239||!nonceBytes)return false;
    if(licensedPlaintext_)return true;  // Explicit public channel: no AES call.
    return keyLen_&&crypt_&&crypt_(context_,key_,keyLen_,nonceBytes,bytes,len);
  }
  bool sealFrame(uint8_t *frame,size_t plainSize,size_t storageSize,size_t outputCapacity,size_t &wireSize) const {
    wireSize=0;
    const size_t overhead=aead_?ChannelAead::TagSize:0;
    if(!originAllowed()||!configured()||!frame||!plainSize||plainSize>239-overhead||
       storageSize<16+plainSize+overhead||outputCapacity<16+plainSize+overhead)return false;
    if(aead_) {
      size_t encrypted=0;
      const auto result=ChannelAead::encrypt(crypt_,context_,key_,keyLen_,read32(frame+4),
        read32(frame),read32(frame+8),frame+16,plainSize,frame+16,storageSize-16,encrypted);
      if(result!=ChannelAeadResult::Success||encrypted!=plainSize+ChannelAead::TagSize)return false;
      wireSize=16+encrypted;return true;
    }
    uint8_t iv[16]{};nonce(read32(frame+4),read32(frame+8),iv);
    const bool ok=transform(frame+16,plainSize,iv);wipe(iv,sizeof(iv));
    if(ok)wireSize=16+plainSize;
    return ok;
  }
  bool openFrame(const uint8_t *frame,size_t wireSize,uint8_t *plain,size_t capacity,
                 size_t &plainSize,bool &backendFailed) const {
    plainSize=0;backendFailed=false;
    if(!configured()||!frame||wireSize<17||wireSize>255||frame[13]!=hash_||!plain)return false;
    if(aead_) {
      const auto result=ChannelAead::decrypt(crypt_,context_,key_,keyLen_,read32(frame+4),
        read32(frame),read32(frame+8),frame+16,wireSize-16,plain,capacity,plainSize);
      backendFailed=result==ChannelAeadResult::BackendFailed;
      return result==ChannelAeadResult::Success;
    }
    const size_t length=wireSize-16;if(length>capacity)return false;
    memcpy(plain,frame+16,length);uint8_t iv[16]{};nonce(read32(frame+4),read32(frame+8),iv);
    const bool ok=transform(plain,length,iv);wipe(iv,sizeof(iv));backendFailed=!ok;
    if(ok)plainSize=length;else wipe(plain,capacity);
    return ok;
  }
  struct MetadataGuard {
    RouteMetadata &value;
    ~MetadataGuard() {wipe(&value,sizeof(value));}
  };
  static uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
  }
  static void write32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i));
  }
  struct Field {
    uint32_t tag=0; uint8_t wire=0; uint64_t value=0;
    const uint8_t *bytes=nullptr; size_t len=0;
  };
  struct Reader {
    const uint8_t *p; size_t n, at=0; bool canonical=false;
    bool varint(uint64_t &v) {
      v=0;
      for(unsigned i=0;i<10;++i) {
        if(at==n) return false;
        const uint8_t b=p[at++];
        if(i==9 && b>1) return false;
        v|=uint64_t(b&0x7f)<<(7*i);
        // Reject redundant high zero groups only on strict application wires.
        // Ten-byte negative int32 encodings end in 0x01 and remain canonical.
        if(!(b&0x80)) return !canonical||!i||(b&0x7f);
      }
      return false;
    }
    bool next(Field &f) {
      uint64_t k=0;
      if(!varint(k)||k>0xffffffffu||(k>>3)==0) return false;
      f=Field{}; f.tag=uint32_t(k>>3); f.wire=uint8_t(k&7);
      switch(f.wire) {
      case 0: return varint(f.value);
      case 1: if(n-at<8) return false; at+=8; return true;
      case 2: {
        uint64_t len=0; if(!varint(len)||len>n-at) return false;
        f.bytes=p+at; f.len=size_t(len); at+=f.len; return true;
      }
      case 5: if(n-at<4) return false; f.value=read32(p+at); at+=4; return true;
      default: return false; // Proto3 does not use groups.
      }
    }
  };
  // XEdDSA is a protobuf Data field (field 10, length-delimited, exactly 64
  // bytes). Only callers that already verified the signature may request a
  // comparison with the unsigned canonical Data body. Unknown/duplicate
  // fields remain byte-for-byte visible to the expected-body comparison.
  static bool matchesExpectedData(const uint8_t *actual,size_t actualSize,
                                  const uint8_t *expected,size_t expectedSize,
                                  bool verifiedSigned) {
    if(!actual||!expected||!expectedSize||expectedSize>239)return false;
    if(actualSize==expectedSize)return !verifiedSigned&&memcmp(actual,expected,expectedSize)==0;
    if(!verifiedSigned||actualSize>239)return false;
    Reader r{actual,actualSize,0,true};uint8_t stripped[239]{};size_t copied=0;bool signature=false;
    while(r.at<r.n) {
      const size_t begin=r.at;Field f;
      if(!r.next(f)) {wipe(stripped,sizeof(stripped));return false;}
      const size_t end=r.at;
      if(f.tag==10) {
        if(signature||f.wire!=2||f.len!=64) {wipe(stripped,sizeof(stripped));return false;}
        signature=true;continue;
      }
      const size_t count=end-begin;
      if(copied>expectedSize||count>expectedSize-copied||count>sizeof(stripped)-copied) {
        wipe(stripped,sizeof(stripped));return false;
      }
      memcpy(stripped+copied,actual+begin,count);copied+=count;
    }
    const bool equal=signature&&copied==expectedSize&&!memcmp(stripped,expected,expectedSize);
    wipe(stripped,sizeof(stripped));return equal;
  }
  // Wire-only gate for a locally produced signed POSITION broadcast. Payload
  // semantics/canonical Position encoding are owned by PkiSigner; this codec
  // requires the exact bounded Data envelope, including one final 64-byte
  // signature, with no query, response, address, or unknown fields.
  static bool signedPositionBroadcastData(const uint8_t *data,size_t size,
                                          const RouteMetadata &metadata) {
    if(!data||!size||size>239||metadata.portnum!=3||metadata.signatureLength!=64||
       !metadata.hasBitfield||metadata.bitfield>1||metadata.wantResponse||metadata.request_id||
       metadata.reply_id||metadata.emoji||!metadata.payloadLength)return false;
    Reader reader{data,size,0,true};uint32_t previous=0;
    bool portSeen=false,payloadSeen=false,bitfieldSeen=false,signatureSeen=false;
    while(reader.at<reader.n) {
      Field field;if(!reader.next(field)||field.tag<=previous||field.tag>10)return false;
      previous=field.tag;
      switch(field.tag) {
      case 1:
        if(field.wire!=0||field.value!=3)return false;portSeen=true;break;
      case 2:
        if(field.wire!=2||!field.len||field.len>233)return false;payloadSeen=true;break;
      case 9:
        if(field.wire!=0||field.value>1)return false;bitfieldSeen=true;break;
      case 10:
        if(field.wire!=2||field.len!=64||reader.at!=reader.n)return false;signatureSeen=true;break;
      default:
        // want_response, dest/source, request/reply/emoji, and unknown fields
        // are not part of a canonical local broadcast envelope.
        return false;
      }
    }
    return portSeen&&payloadSeen&&bitfieldSeen&&signatureSeen;
  }
  static bool routeDiscovery(const uint8_t *p,size_t n,bool strict=false) {
    Reader r{p,n,0,strict}; uint8_t count[4]{};
    while(r.at<r.n) {
      Field f; if(!r.next(f)) return false;
      if(f.tag<1||f.tag>4) {if(strict)return false;continue;}
      uint8_t &c=count[f.tag-1];
      if(f.tag==1||f.tag==3) {
        if(f.wire==5) {if(++c>8) return false;}
        else if(f.wire==2) {
          if(f.len%4||f.len/4>size_t(8-c)) return false;
          c=uint8_t(c+f.len/4);
        } else return false;
      } else {
        auto accept=[&c](uint64_t v) {
          // nanopb stores these INT32 fields in int8_t in the pinned schema.
          const int32_t s=static_cast<int32_t>(uint32_t(v));
          const uint64_t canonical=s<0?uint64_t(int64_t(s)):uint64_t(uint32_t(s));
          if(v!=canonical)return false;
          return ++c<=8&&s>=-128&&s<=127;
        };
        if(f.wire==0) {if(!accept(f.value)) return false;}
        else if(f.wire==2) {
          Reader packed{f.bytes,f.len,0,strict};
          while(packed.at<packed.n) {uint64_t v=0; if(!packed.varint(v)||!accept(v)) return false;}
        } else return false;
      }
    }
    return true;
  }
  static bool routing(const uint8_t *p,size_t n,RouteMetadata &m) {
    Reader r{p,n};
    while(r.at<r.n) {
      Field f; if(!r.next(f)) return false;
      switch(f.tag) {
      case 1: case 2:
        if(f.wire!=2||!routeDiscovery(f.bytes,f.len)) return false;
        m.routingErrorPresent=false; m.routingError=0; break;
      case 3:
        if(f.wire!=0||f.value>0xffffffffu) return false;
        m.routingErrorPresent=true; m.routingError=uint32_t(f.value); break;
      case 4:
        if(f.wire!=2||f.len>8) return false;
        m.ackProofLength=uint8_t(f.len); break;
      default: break;
      }
    }
    m.routingPresent=true;
    return true;
  }
  static bool user(const uint8_t *p,size_t n,RouteMetadata &m,bool allowLicensed=true) {
    Reader r{p,n}; uint16_t meaningful=0; bool licensed=false;
    while(r.at<r.n) {
      Field f; if(!r.next(f)) return false;
      switch(f.tag) {
      case 1: case 2: case 3: {
        const size_t max=f.tag==1?15:(f.tag==2?39:4);
        if(f.wire!=2||f.len>max) return false;
        const uint16_t bit=uint16_t(1u<<f.tag);
        meaningful=uint16_t((meaningful&~bit)|(f.len?bit:0)); break;
      }
      case 4: if(f.wire!=2||f.len!=6) return false; meaningful|=1u<<4; break;
      case 5: case 7: if(f.wire!=0||f.value>0xffffffffu) return false; break;
      case 6: if(f.wire!=0) return false; licensed=f.value!=0; break;
      case 8:
        if(f.wire!=2||f.len>32) return false;
        meaningful=uint16_t((meaningful&~(1u<<8))|(f.len?(1u<<8):0));
        m.publicKeyLength=uint8_t(f.len);
        memset(m.publicKey,0,sizeof(m.publicKey));
        if(f.len) memcpy(m.publicKey,f.bytes,f.len);
        break;
      case 9: if(f.wire!=0) return false; break;
      default: break;
      }
    }
    m.nodeInfoLicensed=licensed;
    // is_licensed is public User metadata, not proof of license ownership and
    // not a property established by channel encryption.
    return meaningful&&(!licensed||allowLicensed);
  }
  static bool supportedTrace(const RouteMetadata &m) {
    return m.portnum==70&&m.traceRouteValid&&m.hasBitfield&&!m.reply_id&&!m.emoji&&
      !m.signatureLength&&((m.wantResponse&&!m.request_id&&(m.bitfield==0||m.bitfield==2))||
      (!m.wantResponse&&m.request_id&&!m.bitfield));
  }
  // Mirror the current PublicApplication::allowed port set, excluding every
  // active moduleBroadcastPort because those use their dedicated typed path.
  // Keep this list synchronized with PublicApplication::allowed when the
  // pinned generated schema changes; the signer performs canonical admission.
  static bool publicApplicationBroadcastPort(uint32_t port) {
    if(moduleBroadcastPort(port))return false;
    if(port>=256&&port<=511)return true;
    switch(port) {
    case 2:case 7:case 8:case 9:case 10:case 11:
    case 32:case 33:case 34:case 35:case 36:case 37:case 38:
    case 64:case 65:case 66:case 68:case 69:case 71:case 72:case 73:
    case 75:case 76:case 77:case 78:case 112:return true;
    default:return false;
    }
  }
public:
  // Compare a received channel frame to complete canonical Data, including any signature bytes.
  bool matchesChannelData(const uint8_t *frame,size_t wireSize,const uint8_t *expected,size_t size)const {
    if(!expected||!size||size>239)return false;
    uint8_t plain[239]{};size_t decoded=0;bool backendFailed=false;
    const bool ok=openFrame(frame,wireSize,plain,sizeof(plain),decoded,backendFailed)&&
      decoded==size&&!memcmp(plain,expected,size);
    wipe(plain,sizeof(plain));return ok;
  }
  explicit MtCodec(MtAesCtr crypt=mtAesCtr,void *context=nullptr):crypt_(crypt),context_(context) {}
  ~MtCodec() {clearKey();}
  MtCodec(const MtCodec &)=delete;
  MtCodec &operator=(const MtCodec &)=delete;
  bool setKey(uint8_t hash,const uint8_t *key,size_t n,bool aead=false) {
    clearKey();
    if(!key||(n!=16&&n!=32)||!crypt_) return false;
    memcpy(key_,key,n); keyLen_=uint8_t(n); hash_=hash; aead_=aead;return true;
  }
  bool setLicensedPlaintext(uint8_t hash) {
    clearKey();licensedPlaintext_=true;hash_=hash;return true;
  }
  void clearKey() {wipe(key_,sizeof(key_)); keyLen_=0; hash_=0;licensedPlaintext_=false;aead_=false;}
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  void restrictOrigins(const ChannelOriginPolicy &policy){originPolicy_=&policy;}
  bool originsRestricted()const{return originPolicy_!=nullptr;}
  bool originAllowed()const{return !originPolicy_||originPolicy_->maySealChannel(key_,keyLen_,licensedPlaintext_);}
#else
  constexpr bool originsRestricted()const{return false;}
  constexpr bool originAllowed()const{return true;}
#endif
  bool hasEncryptedKey() const {return keyLen_==16||keyLen_==32;}
  bool usableConfigured() const {return hasEncryptedKey()||licensedPlaintext_;}
  bool licensedPlaintext() const {return licensedPlaintext_;}
  bool aead() const {return aead_;}
  void setPublicSigning(bool enabled){signPublic_=enabled;}
  bool signsPublic()const{return signPublic_&&!aead_;}
  bool configured() const {return usableConfigured();}
  uint8_t channelHash() const {return hash_;}
  static void nonce(uint32_t from,uint32_t id,uint8_t out[16]) {
    memset(out,0,16); write32(out,id); write32(out+8,from);
  }
  // Decode metadata from a Data protobuf. This helper does not claim a channel
  // was decoded and must not be used to upgrade opaque radio packets to known.
  static bool decodeData(const uint8_t *p,size_t n,RouteMetadata &out,
                         bool allowLicensedNodeInfo=true) {
    out=RouteMetadata{};
    if(!p||n==0||n>239) return false;
    Reader r{p,n}; RouteMetadata m; MetadataGuard erase{m};
    const uint8_t *payload=nullptr; size_t payloadLen=0;
    while(r.at<r.n) {
      Field f; if(!r.next(f)) return false;
      switch(f.tag) {
      case 1:
        if(f.wire!=0||f.value>0xffffffffu) return false;
        m.portnum=uint32_t(f.value); break;
      case 2:
        if(f.wire!=2||f.len>233) return false;
        payload=f.bytes; payloadLen=f.len; break;
      case 3: if(f.wire!=0) return false; m.wantResponse=f.value!=0; break;
      case 4: case 5: if(f.wire!=5) return false; break;
      case 8: if(f.wire!=5) return false; m.emoji=uint32_t(f.value); break;
      case 6: case 7:
        if(f.wire!=5) return false;
        (f.tag==6?m.request_id:m.reply_id)=uint32_t(f.value); break;
      case 9:
        if(f.wire!=0||f.value>255) return false;
        m.hasBitfield=true; m.bitfield=uint8_t(f.value); break;
      case 10:
        if(f.wire!=2||f.len>64) return false;
        m.signatureLength=uint8_t(f.len);
        memset(m.signature,0,sizeof(m.signature));
        if(f.len) memcpy(m.signature,f.bytes,f.len);
        break;
      default: break;
      }
    }
    if(!m.portnum) return false; // Same wrong-key discriminator as Router.cpp.
    if(m.portnum==5) {
      if(!routing(payload,payloadLen,m)) return false;
    } else if(m.portnum==4) m.nodeInfoValid=user(payload,payloadLen,m,allowLicensedNodeInfo);
    else if(m.portnum==70) {
      if(!routeDiscovery(payload,payloadLen,true)) return false;
      m.traceRouteValid=true;
    }
    m.payloadLength=uint8_t(payloadLen);
    if(payloadLen) memcpy(m.payload,payload,payloadLen);
    m.encodedDataSize=uint16_t(n); out=m; return true;
  }
  // Channel-zero unicast is PKI-shaped in the stock wire format. Keep the
  // default fail-closed; the owner may opt in only after it proves this frame
  // belongs to the selected encrypted channel whose legitimate hash is zero.
  bool decode(const uint8_t *frame,size_t n,RouteMetadata &out,
              bool allowKeyedZeroUnicast=false) const {
    out=RouteMetadata{};
    if(!configured()||!frame||n<17||n>255||frame[13]!=hash_) return false;
    // Channel zero unicasts are PKI-shaped. Never try endpoint/private-message
    // decryption or allow accidental CTR parsing to populate trusted history.
    if(!licensedPlaintext_&&frame[13]==0&&read32(frame)!=0xffffffffu&&
       !(allowKeyedZeroUnicast&&hasEncryptedKey())) return false;
    out.decodeRejected=true;
    if(!read32(frame+4)||read32(frame+4)==0xffffffffu||!read32(frame+8)) return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    RouteMetadata m; MetadataGuard erase{m};
    const bool cryptoOk=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed);
    out.backendFailed=backendFailed;
    const bool ok=cryptoOk&&decodeData(plain,plainSize,m,true);
    wipe(plain,sizeof(plain));
    if(!ok) return false;
    m.channelDecoded=true; out=m; return true;
  }
  // Local cache comparison only: this checks the exact Data bytes carried by
  // an already-produced frame against its canonical unsigned source, removing
  // only the XEdDSA field when the caller's own queue metadata says it signed.
  // It does not verify that signature and is never an RF admission decision.
  bool matchesEncodedData(const uint8_t *frame,size_t n,const uint8_t *expected,
                          size_t expectedSize,bool signedByLocalProducer) const {
    if(!configured()||!frame||n<17||n>255||frame[13]!=hash_||!expected||
       !expectedSize||expectedSize>239)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool transformed=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed);
    RouteMetadata metadata{};
    const bool decoded=transformed&&decodeData(plain,plainSize,metadata);
    const bool signatureOk=decoded&&(signedByLocalProducer?metadata.signatureLength==64:
                                                        metadata.signatureLength==0);
    const bool exact=signatureOk&&matchesExpectedData(plain,plainSize,expected,expectedSize,
                                                       signedByLocalProducer);
    wipe(plain,sizeof(plain));wipe(&metadata,sizeof(metadata));
    return exact;
  }
  // Bounded companion channel origination. Zero hop, no RF ACK request and
  // no relay involvement. Caller supplies encoded Data and handles RF policy.
  bool encodeZeroHopData(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                         uint8_t *out,size_t cap,size_t &len) const {
    len=0;RouteMetadata m;
    if(!configured()||!out||!data||n>239||cap<n+16||ours<=1||ours>=0xfffffffeu||!to||!id||
      !decodeData(data,n,m)||!m.hasBitfield||m.wantResponse||m.signatureLength)return false;
    uint8_t frame[255]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
    size_t wireSize=0;const bool ok=sealFrame(frame,n,sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));wipe(&m,sizeof(m));return ok;
  }
#if defined(CONFIG_MT_GPIO_MODULES)
  // Exact reconstructed GPIO Data shape, after Service's channel/signature
  // gate. Encryption proves byte equality only, never sender authorization.
  bool validateGpioModuleData(const uint8_t *frame,size_t size,const uint8_t *data,size_t n)const {
    if(!configured()||!frame||!data||size<17||size>255||n>239||frame[13]!=hash_)return false;
    return matchesChannelData(frame,size,data,n);
  }
#endif
  // Exact reconstructed Public NodeInfo Data. This admits only canonical
  // node-info announcements/requests already classified by decodeData; the
  // caller still owns signature verification and packet direction policy.
  bool validatePublicNodeInfoData(const uint8_t *frame,size_t size,const uint8_t *data,size_t n)const {
    RouteMetadata m;MetadataGuard erase{m};
    if(!configured()||!frame||!data||size<17||size>255||n>239||frame[13]!=hash_||
       !decodeData(data,n,m)||m.portnum!=4||!m.nodeInfoValid||!m.hasBitfield||
       (m.bitfield&~3u)||m.request_id||m.reply_id||m.emoji)return false;
    return matchesChannelData(frame,size,data,n);
  }
#if defined(CONFIG_MT_STORE_FORWARD_PSRAM)
  // Compare a caller-reconstructed complete Data encoding with the received
  // channel ciphertext. This establishes shape only, never sender identity.
  bool validateStoreForwardData(const uint8_t *frame,size_t size,const uint8_t *data,size_t n)const {
    if(!configured()||!frame||!data||size<17||size>255||n>239||frame[13]!=hash_)return false;
    return matchesChannelData(frame,size,data,n);
  }
#endif
  // Read-only admission for independently verified Public module broadcasts.
  // The application reconstructs canonical Data including the signature; this
  // proves those exact bytes were received under the original source/ID nonce.
  // No caller verification flag or signed origination is accepted by this API.
  bool validateSignedModuleData(const uint8_t *frame,size_t n,
                                const uint8_t *expected,size_t expectedSize)const {
    RouteMetadata metadata;MetadataGuard erase{metadata};
    if(!configured()||!frame||n<17||n>255||!expected||!expectedSize||expectedSize>239||
       frame[13]!=hash_||read32(frame)!=0xffffffffu||read32(frame+4)<=1||
       read32(frame+4)>=0xfffffffeu||!read32(frame+8)||
       !decodeData(expected,expectedSize,metadata)||!publicSignedModulePort(metadata.portnum)||
       (metadata.portnum==ModuleAudioPort&&!mt::audio::AudioWire::validPayload(metadata.payload,metadata.payloadLength))||
       (metadata.portnum==ModulePaxcounterPort&&!mt::audio::PaxcountWire::validPayload(metadata.payload,metadata.payloadLength))||
       metadata.signatureLength!=64||!metadata.hasBitfield||metadata.bitfield>1||
       metadata.wantResponse||metadata.request_id)return false;
    return matchesChannelData(frame,n,expected,expectedSize);
  }
  // Exact Public unicast traceroute request. This is the sole ordinary
  // traceroute request allowed to carry Data.want_response. Service and
  // Endpoint still own destination, channel, hop, TX and correlation policy.
  bool encodeTraceRouteRequest(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                               uint8_t *out,size_t cap,size_t &len) const {
    len=0;RouteMetadata m;MetadataGuard erase{m};
    if(!configured()||!out||!data||!n||n>239||cap<n+16||ours<=1||ours>=0xfffffffeu||
      to<=1||to>=0xfffffffeu||to==ours||!id||!decodeData(data,n,m)||m.portnum!=70||
      !m.traceRouteValid||!m.hasBitfield||(m.bitfield!=0&&m.bitfield!=2)||!m.wantResponse||m.request_id||
      m.reply_id||m.emoji||m.signatureLength)return false;
    uint8_t frame[255]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
    size_t wireSize=0;const bool ok=sealFrame(frame,n,sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
#if defined(CONFIG_MT_GPIO_MODULES)
  // GPIO requests are a single bounded stock application shape; this does not
  // relax the generic application encoder or accept any other port-2 form.
  bool encodeGpioRequest(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                         uint8_t *out,size_t cap,size_t &len) const;
#endif
  // Exact empty Public position query: no caller coordinates or arbitrary
  // application request can enter this narrow encoder.
  bool encodePositionQuery(uint32_t ours,uint32_t to,uint32_t id,
                           uint8_t *out,size_t cap,size_t &len) const {
    len=0;constexpr uint8_t data[]={8,3,24,1,72,2};
    if(!usableConfigured()||!out||cap<sizeof(data)+16||ours<=1||ours>=0xfffffffeu||
      to<=1||to>=0xfffffffeu||to==ours||!id)return false;
    uint8_t frame[22+ChannelAead::TagSize]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,sizeof(data));
    size_t wireSize=0;const bool ok=sealFrame(frame,sizeof(data),sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
  // Exact empty NeighborInfo selector, with the same frame/CTR contract.
  bool encodeNeighborQuery(uint32_t ours,uint32_t to,uint32_t id,
                           uint8_t *out,size_t cap,size_t &len) const {
    len=0;constexpr uint8_t data[]={8,71,24,1,72,2};
    if(!usableConfigured()||!out||cap<sizeof(data)+16||ours<=1||ours>=0xfffffffeu||
      to<=1||to>=0xfffffffeu||to==ours||!id)return false;
    uint8_t frame[22+ChannelAead::TagSize]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,sizeof(data));
    size_t wireSize=0;const bool ok=sealFrame(frame,sizeof(data),sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
  // Exact empty pinned telemetry variant selector; no caller measurements are sent.
  bool encodeTelemetryQuery(uint32_t ours,uint32_t to,uint32_t id,
                            uint8_t *out,size_t cap,size_t &len,bool localStatsQuery=false,uint8_t requestedVariant=0) const {
    len=0;const uint8_t variant=requestedVariant?requestedVariant:uint8_t(localStatsQuery?6:2);
    if(!((variant>=2&&variant<=9)||variant==11)||(localStatsQuery&&variant!=6))return false;
    uint8_t data[]={8,67,18,2,uint8_t((variant<<3)|2),0,24,1,72,2};
    if(!usableConfigured()||!out||cap<sizeof(data)+16||ours<=1||ours>=0xfffffffeu||
      to<=1||to>=0xfffffffeu||to==ours||!id)return false;
    uint8_t frame[26+ChannelAead::TagSize]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,sizeof(data));
    size_t wireSize=0;const bool ok=sealFrame(frame,sizeof(data),sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
  // Re-encoded application/Data supplied by the bounded response validator must
  // exactly match decrypted bytes. Duplicate/unknown/noncanonical envelope
  // fields cannot borrow an otherwise valid response's request ID.
  bool validatePublicQueryReply(const uint8_t *frame,size_t n,
                               const uint8_t *expected,size_t expectedSize,uint32_t responsePort,
                               bool verifiedSigned=false) const {
    RouteMetadata m;MetadataGuard erase{m};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_&&responsePort!=3)||
      !frame||n<17||n>255||!expected||expectedSize>239||
      frame[13]!=hash_||!decodeData(expected,expectedSize,m)||(responsePort!=3&&responsePort!=67&&responsePort!=71&&responsePort!=78)||(m.portnum!=responsePort&&m.portnum!=5)||!m.request_id||
      m.wantResponse||m.reply_id||m.emoji||!m.hasBitfield||(m.bitfield&~1u))return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool transformed=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed);
    RouteMetadata actual;MetadataGuard actualErase{actual};
    const bool decoded=transformed&&decodeData(plain,plainSize,actual);
    const bool signatureOk=verifiedSigned?actual.signatureLength==64:actual.signatureLength==0;
    const bool ok=decoded&&signatureOk&&matchesExpectedData(plain,plainSize,expected,expectedSize,verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  // Exact canonical Public DeviceMetrics selector; no caller metrics or extra fields.
  bool validateTelemetryRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    static constexpr uint8_t expected[]={8,67,18,2,18,0,24,1,72,2};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  bool validateLocalStatsRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    static constexpr uint8_t expected[]={8,67,18,2,0x32,0,24,1,72,2};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  bool validateEnvironmentRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    static constexpr uint8_t expected[]={8,67,18,2,0x1a,0,24,1,72,2};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  bool validatePowerRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    static constexpr uint8_t expected[]={8,67,18,2,0x2a,0,24,1,72,2};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  static uint8_t supplementalTelemetryVariant(const uint8_t *payload,size_t size){
    if(!payload||size!=2||payload[1])return 0;
    return payload[0]==0x22?4:payload[0]==0x3a?7:0;
  }
  bool validateSupplementalTelemetryRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    uint8_t expected[]={8,67,18,2,0,0,24,1,72,2};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool decoded=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed);
    const uint8_t variant=decoded?supplementalTelemetryVariant(plain+4,2):0;
    expected[4]=variant==4?0x22:0x3a;
    const bool ok=variant&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  bool validatePositionRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    static constexpr uint8_t expected[]={8,3,24,1,72,2};
    if(!usableConfigured()||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
  bool validateNeighborRequest(const uint8_t *frame,size_t n,bool verifiedSigned=false)const {
    static constexpr uint8_t expected[]={8,71,24,1,72,2};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
#if defined(CONFIG_MT_PAXCOUNTER)
  bool validatePaxRequest(const uint8_t *frame,size_t n,uint8_t bitfield,bool verifiedSigned=false)const {
    if((bitfield&~1u)!=2)return false;
    const uint8_t expected[]={8,34,24,1,72,bitfield};
    if(!usableConfigured()||(verifiedSigned&&!licensedPlaintext_)||!frame||n<16+sizeof(expected)||n>255||frame[13]!=hash_)return false;
    uint8_t plain[239]{};size_t plainSize=0;bool backendFailed=false;
    const bool ok=openFrame(frame,n,plain,sizeof(plain),plainSize,backendFailed)&&matchesExpectedData(plain,plainSize,expected,sizeof(expected),verifiedSigned);
    wipe(plain,sizeof(plain));return ok;
  }
#endif
  bool validateNeighborQueryReply(const uint8_t *frame,size_t n,const uint8_t *expected,size_t size,bool verifiedSigned=false) const {
    return validatePublicQueryReply(frame,n,expected,size,71,verifiedSigned);
  }
  bool validatePositionQueryReply(const uint8_t *frame,size_t n,const uint8_t *expected,size_t size,uint32_t responsePort=3,bool verifiedSigned=false) const {
    return (responsePort==3||responsePort==78)&&validatePublicQueryReply(frame,n,expected,size,responsePort,verifiedSigned);
  }
  bool validateTelemetryQueryReply(const uint8_t *frame,size_t n,const uint8_t *expected,size_t size,bool verifiedSigned=false) const {
    return validatePublicQueryReply(frame,n,expected,size,67,verifiedSigned);
  }
  // Prove that an RF frame contains exactly the caller's supported canonical
  // Data bytes. This is used for loop observations that must never be forwarded
  // but still require the same admission strength as a rewritten copy.
  bool validateTraceRouteData(const uint8_t *frame,size_t n,
                              const uint8_t *expected,size_t expectedSize) const {
    RouteMetadata metadata;MetadataGuard erase{metadata};
    if(!usableConfigured()||!frame||n<17||n>255||!expected||!expectedSize||expectedSize>239||
       frame[13]!=hash_||read32(frame)<=1||read32(frame)>=0xfffffffeu||
       read32(frame+4)<=1||read32(frame+4)>=0xfffffffeu||!read32(frame+8)||
       !decodeData(expected,expectedSize,metadata)||!supportedTrace(metadata))return false;
    return matchesChannelData(frame,n,expected,expectedSize);
  }
  // Rewrite only an exact supported canonical Public traceroute Data body.
  // The caller supplies independently normalized RouteDiscovery bytes; this
  // method proves the expected old Data is byte-identical before replacing it
  // under the original source/id nonce. Header routing fields remain untouched.
  bool rewriteTraceRouteData(const uint8_t *frame,size_t n,
                             const uint8_t *expected,size_t expectedSize,
                             const uint8_t *replacement,size_t replacementSize,
                             uint8_t *out,size_t capacity,size_t &size) const {
    size=0;RouteMetadata before,after;MetadataGuard beforeErase{before},afterErase{after};
    if(!usableConfigured()||!frame||n<17||n>255||!expected||!expectedSize||!replacement||!replacementSize||
       replacementSize>239||expectedSize>239||!out||capacity<replacementSize+16||
       frame[13]!=hash_||read32(frame)<=1||read32(frame)>=0xfffffffeu||
       read32(frame+4)<=1||read32(frame+4)>=0xfffffffeu||!read32(frame+8)||
       !decodeData(expected,expectedSize,before)||!decodeData(replacement,replacementSize,after))return false;
    if(!supportedTrace(before)||!supportedTrace(after)||before.wantResponse!=after.wantResponse||
       before.request_id!=after.request_id)return false;
    if(!matchesChannelData(frame,n,expected,expectedSize))return false;
    memcpy(out,frame,16);memcpy(out+16,replacement,replacementSize);
    const bool ok=sealFrame(out,replacementSize,capacity,capacity,size);
    if(!ok)wipe(out,capacity);
    return ok;
  }
  // Only a locally generated, independently verified signed NodeInfo may use
  // this encoder. Other applications retain encodeZeroHopData's signature ban.
  // The runtime checks the complete signature before entering this narrow API.
  bool encodeSignedNodeInfo(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                           uint8_t *out,size_t cap,size_t &len) const {
    len=0;RouteMetadata m;MetadataGuard erase{m};
    if(!configured()||!out||!data||n>239||cap<n+16||ours<=1||ours>=0xfffffffeu||to<=1||to==0xfffffffeu||!id||
      !decodeData(data,n,m)||m.portnum!=4||!m.nodeInfoValid||m.publicKeyLength!=32||m.signatureLength!=64||!m.hasBitfield)return false;
    uint8_t frame[255]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
    size_t wireSize=0;const bool ok=sealFrame(frame,n,sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
  // Explicit shared-channel Admin response envelope. The typed owner supplies
  // channel/token authority and canonical payload admission; this primitive
  // never accepts PKI/signature claims or licensed plaintext.
  bool encodeAdminChannelResponse(uint32_t ours,uint32_t to,uint32_t id,
      const uint8_t *data,size_t n,uint8_t *out,size_t capacity,size_t &size)const {
    size=0;
    if(!configured()||licensedPlaintext_||(keyLen_!=16&&keyLen_!=32)||
       !data||!n||n>239||!out||capacity<n+16||ours<=1||ours>=0xfffffffeu||
       to<=1||to>=0xfffffffeu||to==ours||!id)return false;
    Reader reader{data,n,0,true};uint32_t previous=0;
    bool port=false,payload=false,request=false,bitfield=false;
    while(reader.at<reader.n) {
      Field field;if(!reader.next(field)||field.tag<=previous)return false;
      previous=field.tag;
      switch(field.tag) {
      case 1:if(field.wire!=0||field.value!=6)return false;port=true;break;
      case 2:if(field.wire!=2||!field.len||field.len>233)return false;payload=true;break;
      case 6:if(field.wire!=5||!field.value)return false;request=true;break;
      case 9:if(field.wire!=0||field.value>1)return false;bitfield=true;break;
      default:return false;
      }
    }
    if(!port||!payload||!request||!bitfield)return false;
    uint8_t frame[255]{};
    write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
    size_t wireSize=0;const bool encoded=sealFrame(frame,n,sizeof(frame),capacity,wireSize);
    if(encoded){memcpy(out,frame,wireSize);size=wireSize;}
    wipe(frame,sizeof(frame));return encoded;
  }
  // Local signer output only. Restrict the new API to signable broadcasts;
  // generic channel encoding and its caller-signature rejection stay intact.
  bool encodeSignedApplication(uint32_t ours,uint32_t id,const uint8_t *data,size_t n,
                               uint8_t *out,size_t cap,size_t &len)const {
    len=0;RouteMetadata m;MetadataGuard erase{m};
    if(!configured()||!out||!data||n>239||cap<n+16||ours<=1||ours>=0xfffffffeu||!id||
       !decodeData(data,n,m)||(m.portnum!=1&&!moduleBroadcastPort(m.portnum)&&m.portnum!=3&&
         !publicApplicationBroadcastPort(m.portnum))||
       (m.portnum==3&&!signedPositionBroadcastData(data,n,m))||
       (m.portnum==ModuleAudioPort&&!mt::audio::AudioWire::validPayload(m.payload,m.payloadLength))||
       (m.portnum==ModulePaxcounterPort&&!mt::audio::PaxcountWire::validPayload(m.payload,m.payloadLength))||m.signatureLength!=64||
       !m.hasBitfield||m.wantResponse||m.request_id||(m.bitfield&~1u))return false;
    uint8_t frame[255]{};write32(frame,0xffffffffu);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
    size_t wireSize=0;const bool ok=sealFrame(frame,n,sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
  // The producer validates the canonical typed request or reply before reserving an ID.
  // Shared encrypted channels only admit the directed public Position surface here.
  bool encodeProducedUnicast(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                             uint8_t *out,size_t cap,size_t &len,bool signedData)const {
    len=0;RouteMetadata m;MetadataGuard erase{m};
    if(!configured()||!out||!data||n>239||cap<n+16||ours<=1||ours>=0xfffffffeu||
       to<=1||to>=0xfffffffeu||to==ours||!id||!decodeData(data,n,m)||
       m.signatureLength!=(signedData?64:0)||(!licensedPlaintext_&&(m.portnum!=3||!signedData)))return false;
    uint8_t frame[255]{};write32(frame,to);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);memcpy(frame+16,data,n);
    const bool ok=sealFrame(frame,n,sizeof(frame),cap,len);if(ok)memcpy(out,frame,len);
    wipe(frame,sizeof(frame));return ok;
  }
  bool encodeSignedLicensedUnicast(uint32_t ours,uint32_t to,uint32_t id,const uint8_t *data,size_t n,
                                   uint8_t *out,size_t cap,size_t &len)const {
    len=0;return licensedPlaintext_&&encodeProducedUnicast(ours,to,id,data,n,out,cap,len,true);
  }
  // Exact discovery-only origination. Zero-hop avoids pretending that this is
  // a forwarded packet at the native gate. It never requests or synthesizes ACKs.
  bool encodeZeroHopNodeInfo(uint32_t ours,uint32_t id,const uint8_t *payload,size_t n,
                            uint8_t *out,size_t cap,size_t &len) const {
    len=0;RouteMetadata metadata;
    if(!configured()||!out||!payload||n>127||cap<n+22||ours<=1||ours>=0xfffffffeu||!id||
       !user(payload,n,metadata,licensedPlaintext_))return false;
    uint8_t frame[149+ChannelAead::TagSize]{};write32(frame,0xffffffffu);write32(frame+4,ours);write32(frame+8,id);
    frame[13]=hash_;frame[15]=uint8_t(ours);
    frame[16]=0x08;frame[17]=4;frame[18]=0x12;frame[19]=uint8_t(n);memcpy(frame+20,payload,n);
    // Modern Meshtastic originators always include optional Data.bitfield.
    // Its presence distinguishes a valid zero-hop packet from pre-hop firmware
    // at the pinned receiver's post-decode acceptance gate. Zero requests no
    // response and grants no MQTT forwarding permission.
    frame[20+n]=0x48;frame[21+n]=0;
    size_t wireSize=0;const bool ok=sealFrame(frame,n+6,sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));wipe(&metadata,sizeof(metadata));return ok;
  }
  // Pure wire encoder, not a request to transmit and not proof of endpoint
  // receipt. A caller must first establish official zero-hop ACK eligibility.
  // This candidate does not connect this helper to the radio or routing core.
  bool encodeZeroHopAck(uint32_t ours,uint32_t to,uint32_t id,uint32_t request,
                        uint8_t *out,size_t cap,size_t &len) const {
    len=0;
    if(!configured()||!out||cap<27||!ours||ours==0xffffffffu||!to||to==0xffffffffu||!id||!request) return false;
    uint8_t frame[27+ChannelAead::TagSize]{}; write32(frame,to); write32(frame+4,ours); write32(frame+8,id);
    frame[13]=hash_; frame[15]=uint8_t(ours);
    // Data(portnum=ROUTING_APP, payload=Routing(error_reason=NONE), request_id).
    // error_reason is an explicit oneof value, so zero must be serialized.
    const uint8_t data[7]={0x08,0x05,0x12,0x02,0x18,0x00,0x35};
    memcpy(frame+16,data,sizeof(data)); write32(frame+23,request);
    size_t wireSize=0;const bool ok=sealFrame(frame,11,sizeof(frame),cap,wireSize);
    if(ok){memcpy(out,frame,wireSize);len=wireSize;}
    wipe(frame,sizeof(frame));return ok;
  }
};
} // namespace bench
