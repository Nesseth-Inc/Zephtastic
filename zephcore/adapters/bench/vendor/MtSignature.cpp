// SPDX-License-Identifier: GPL-3.0-only
#include "MtSignature.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

namespace bench {
uint32_t MtSignature::read32(const uint8_t *p) {
  return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
void MtSignature::write32(uint8_t *p,uint32_t v) {
  for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i));
}
void MtSignature::clear() { for(auto &entry:entries_) entry=Entry{}; }
void MtSignature::forgetPublicKey(uint32_t node) {
  for(auto &entry:entries_)if(entry.node==node)entry=Entry{};
}
unsigned MtSignature::keyCount(uint32_t now) const {
  unsigned n=0;
  for(const auto &e:entries_) n+=live(e,now);
  return n;
}
bool MtSignature::trustPublicKey(uint32_t node,const uint8_t key[32],uint32_t now) {
  if(node<2||node==0xffffffffu||!key) return false;
  Entry *slot=nullptr;
  for(auto &e:entries_) {
    if(live(e,now)) {
      if(e.node==node) {
        if(memcmp(e.curve,key,32)) return false;
        e.seen=now; return true;
      }
    } else if(!slot) slot=&e;
  }
  uint8_t ed[32]{};
  if(!slot||!admissiblePublicKey(key,ed)) return false;
  slot->node=node; slot->seen=now;
  memcpy(slot->curve,key,32); memcpy(slot->ed,ed,32);
  return true;
}
uint32_t MtSignature::nodeId(const uint8_t publicKey[32]) {
  if(!publicKey) return 0;
  // Standard reflected CRC-32/ISO-HDLC, matching pinned ErriezCRC32 1.0.1
  // crc32Buffer: initial/final XOR 0xffffffff, polynomial 0xedb88320.
  uint32_t crc=0xffffffffu;
  for(unsigned i=0;i<32;++i) {
    crc^=publicKey[i];
    for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
  }
  return crc^0xffffffffu;
}
size_t MtSignature::signingBuffer(uint32_t from,uint32_t id,uint32_t to,
                                 const RouteMetadata &m,uint8_t *out,size_t cap) {
  const size_t n=SigningHeaderLength+m.payloadLength;
  if(!out||m.payloadLength>sizeof(m.payload)||cap<n) return 0;
  out[0]=1;
  const uint32_t fields[8]={from,id,to,m.portnum,m.request_id,m.reply_id,m.emoji,
                            m.hasBitfield?uint32_t(m.bitfield):0u};
  for(unsigned i=0;i<8;++i) write32(out+1+4*i,fields[i]);
  out[33]=uint8_t((m.wantResponse?1:0)|(m.hasBitfield?2:0));
  if(m.payloadLength) memcpy(out+SigningHeaderLength,m.payload,m.payloadLength);
  return n;
}
bool MtSignature::admissiblePublicKey(const uint8_t curve[32],uint8_t ed[32]) {
  if(!curve||!ed||curve[31]&0x80) return false;
  // Canonical field encoding u < 2^255-19. Reject aliases instead of allowing
  // a different public byte string/CRC to represent the same verification key.
  bool less=false;
  for(int i=31;i>=0;--i) {
    const uint8_t p=i==31?0x7f:(i==0?0xed:0xff);
    if(curve[i]!=p) {less=curve[i]<p; break;}
  }
  if(!less) return false;
  const uint8_t publicValidationScalar[32]={0}; // clamped by library; public
  uint8_t product[32]{};
  crypto_x25519(product,publicValidationScalar,curve);
  uint8_t nonzero=0;
  for(uint8_t b:product) nonzero|=b;
  crypto_wipe(product,sizeof(product));
  if(!nonzero) return false;
  crypto_x25519_to_eddsa(ed,curve);
  // X25519 also accepts points on the Montgomery twist. A signature key must
  // decode on Edwards25519. Existing equation checking with public values
  // s=0,h=0,R=identity succeeds for every decodable A, and fails when A does
  // not decode. This validates the point only; it authenticates no packet.
  const uint8_t identityEquation[64]={1},zeroChallenge[32]={0};
  return crypto_eddsa_check_equation(identityEquation,ed,zeroChallenge)==0;
}
bool MtSignature::publicIdentity(uint32_t node,uint32_t now,uint8_t out[32])const {
  if(!out)return false;
  memset(out,0,32);
  for(const auto &entry:entries_)if(entry.node==node&&live(entry,now)){
    memcpy(out,entry.curve,32);return true;
  }
  return false;
}

MtSignature::Result MtSignature::apply(const uint8_t *frame,size_t n,
                                      RouteMetadata &m,uint32_t now,bool allowResolvedZeroUnicast) {
  // Never trust caller-supplied verification flags, even across packet reuse.
  m.signatureVerified=m.signatureQuarantined=m.signatureRejected=false;
  auto reject=[&m]() {m.signatureRejected=true; return Result::Rejected;};
  auto quarantine=[&m]() {m.signatureQuarantined=true; return Result::Quarantined;};
  if(!m.channelDecoded||m.decodeRejected||m.backendFailed||!frame||n<17||n>255)
    return reject();
  const uint32_t from=read32(frame+4),id=read32(frame+8),to=read32(frame);
  if(from<2||from==0xffffffffu||!id||m.payloadLength>sizeof(m.payload)) return reject();
  // A signature checker is never a channel-zero PKI decryptor. A resolved
  // shared-channel context needs an explicit owner opt-in; metadata alone
  // cannot relax the default rejection used by all other callers.
  if(frame[13]==0&&to!=0xffffffffu&&
     (!allowResolvedZeroUnicast||!m.channelContext)) return reject();
  if(!m.signatureLength) return Result::Unsigned;
  if(m.signatureLength!=64) return reject();

  Entry *known=nullptr,*slot=nullptr;
  for(auto &e:entries_) {
    if(live(e,now)) {if(e.node==from) known=&e;}
    else if(!slot) slot=&e;
  }
  uint8_t candidateEd[32]{};
  const uint8_t *ed=known?known->ed:candidateEd;
  if(!known) {
    if(m.portnum!=4) return quarantine();
    if(!m.nodeInfoValid||m.publicKeyLength!=32||nodeId(m.publicKey)!=from||
       !admissiblePublicKey(m.publicKey,candidateEd)) return reject();
  } else if(m.portnum==4 && m.publicKeyLength &&
            (m.publicKeyLength!=32||memcmp(m.publicKey,known->curve,32))) {
    // A valid signature by the old key cannot overwrite its authoritative key.
    return reject();
  }
  uint8_t message[SigningHeaderLength+233]{};
  const size_t len=signingBuffer(from,id,to,m,message,sizeof(message));
  const bool valid=len && crypto_ed25519_check(m.signature,ed,message,len)==0;
  crypto_wipe(message,sizeof(message));
  if(!valid) return reject();
  if(!known) {
    // No live-key eviction: capacity exhaustion cannot erase proven authority.
    if(!slot) return reject();
    slot->node=from; memcpy(slot->curve,m.publicKey,32); memcpy(slot->ed,candidateEd,32);
    known=slot;
  }
  known->seen=now;
  m.signatureVerified=true;
  return Result::Verified;
}
} // namespace bench
