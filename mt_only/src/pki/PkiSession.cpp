// SPDX-License-Identifier: GPL-3.0-only
#include "PkiSession.h"
#include "monocypher.h"
#include <string.h>
namespace mt::pki {
namespace {
struct Scalar {uint8_t bytes[32]{};~Scalar(){crypto_wipe(bytes,32);}};
struct Extra {uint32_t value=0;~Extra(){crypto_wipe(&value,sizeof(value));}};
bool nonzero(const uint8_t *p){uint8_t n=0;for(unsigned i=0;i<32;++i)n|=p[i];return n!=0;}
bool node(uint32_t n){return n>1 && n<0xfffffffeu;}
}
bool PkiSession::ready()const {
  if(state_==State::Ready&&keys_.active(key_))return true;
  if(sharedKey_.valid)crypto_wipe(&sharedKey_,sizeof(sharedKey_));
  return false;
}
bool PkiSession::load(uint8_t scalar[32]) {
  struct CopyOperation:PrivateKeyOperation {
    uint8_t *out;bool invoked=false;
    explicit CopyOperation(uint8_t *p):out(p){}
    bool apply(const uint8_t value[32])override {
      if(invoked || !value)return false;
      invoked=true;memcpy(out,value,32);return true;
    }
  } operation(scalar);
  if(keys_.active(key_)&&keys_.withPrivate(key_,operation)&&operation.invoked&&nonzero(scalar)&&
     !(scalar[0]&7)&&(scalar[31]&0xc0)==0x40){
    Scalar derived;crypto_x25519_public_key(derived.bytes,scalar);
    if(crypto_verify32(derived.bytes,localPublic_)==0)return true;
  }
  crypto_wipe(&sharedKey_,sizeof(sharedKey_));return false;
}
bool PkiSession::open(KeyRef key,uint32_t local,const uint8_t publicKey[32],
                       uint32_t peer,const uint8_t peerKey[32]) {
  if(state_!=State::Empty || !key.generation || !node(local) ||
     !node(peer) || local==peer || !publicKey || !peerKey || !nonzero(publicKey) ||
     !nonzero(peerKey) || crypto_verify32(publicKey,peerKey)==0)return false;
  key_=key;local_=local;peer_=peer;
  memcpy(localPublic_,publicKey,32);memcpy(peerPublic_,peerKey,32);
  Scalar scalar;
  if(!load(scalar.bytes)){close();return false;}
  // Reject low-order peer keys before consuming a nonce. X25519 here does not
  // establish identity/trust; it only checks a usable nonzero shared secret.
  Scalar product;crypto_x25519(product.bytes,scalar.bytes,peerPublic_);
  if(!nonzero(product.bytes)){close();return false;}
  state_=State::Ready;return true;
}
void PkiSession::close() {
  state_=State::Closed;
  crypto_wipe(&sharedKey_,sizeof(sharedKey_));
  crypto_wipe(slots_,sizeof(slots_));crypto_wipe(sent_,sizeof(sent_));
  crypto_wipe(received_,sizeof(received_));crypto_wipe(localPublic_,32);
  crypto_wipe(peerPublic_,32);key_={};local_=peer_=0;sentCount_=receivedCount_=0;
}
SessionResult PkiSession::send(uint32_t id,const uint8_t *data,size_t n,PkiFrame &out) {
  if(!ready())return SessionResult::NotReady;
  if(!id || !data || !n || n>experimental::PkiCrypto::MaxPlaintext)return SessionResult::InvalidArgument;
  const bool pairs=nonces_.persistentNoncePairs();
  const bool persistent=nonces_.persistentIds()||pairs;
  if(pairs)for(const auto &pending:slots_)
    if(pending.active&&pending.frame.id==id)return SessionResult::NonceReused;
  if(!persistent){
    for(size_t i=0;i<sentCount_;++i)if(sent_[i]==id)return SessionResult::NonceReused;
    if(sentCount_==NonceLimit)return SessionResult::NonceExhausted;
  }
  Slot *slot=nullptr;for(auto &s:slots_)if(!s.active){slot=&s;break;}
  if(!slot)return SessionResult::NoSlot;
  if(!persistent)sent_[sentCount_++]=id;
  // Persistent providers burn before returning any nonce; uncertainty freezes
  // their domain. Other providers retain the old irrevocable RAM reservation.
  Extra extra;
  if(!nonces_.reserve(key_,localPublic_,local_,id,extra.value))return SessionResult::NonceUnavailable;
  Scalar scalar;
  if(!load(scalar.bytes))return SessionResult::KeyUnavailable;
  PkiFrame frame{};frame.from=local_;frame.to=peer_;frame.id=id;
  const bool ok=crypto_.encrypt(scalar.bytes,peerPublic_,local_,id,extra.value,data,n,
                                frame.bytes,sizeof(frame.bytes),frame.size);
  if(!ok){crypto_wipe(&frame,sizeof(frame));return SessionResult::CryptoFailure;}
  slot->frame=frame;slot->active=true;out=frame;
  crypto_wipe(&frame,sizeof(frame));return SessionResult::Ok;
}
SessionResult PkiSession::retry(uint32_t id,PkiFrame &out)const {
  if(!ready())return SessionResult::NotReady;
  for(const auto &s:slots_)if(s.active && s.frame.id==id){out=s.frame;return SessionResult::Ok;}
  return SessionResult::NotPending;
}
bool PkiSession::retire(uint32_t id) {
  if(!ready())return false;
  for(auto &s:slots_)if(s.active && s.frame.id==id){crypto_wipe(&s,sizeof(s));return true;}
  return false;
}
SessionResult PkiSession::receive(const PkiFrame &frame,uint8_t *out,size_t cap,size_t &len) {
  len=0;if(!ready())return SessionResult::NotReady;
  if(frame.from!=peer_ || frame.to!=local_)return SessionResult::PeerMismatch;
  if(!frame.id || !out || frame.size<=12 || frame.size>239 || cap<frame.size-12)return SessionResult::InvalidArgument;
  for(size_t i=0;i<receivedCount_;++i)if(received_[i]==frame.id)return SessionResult::Replay;
  if(receivedCount_==ReceiveLimit)return SessionResult::RxExhausted;
  const auto result=authenticate(frame,out,cap,len);
  if(result==SessionResult::Ok)received_[receivedCount_++]=frame.id;
  return result;
}
SessionResult PkiSession::authenticate(const PkiFrame &frame,uint8_t *out,size_t cap,size_t &len) {
  len=0;if(!ready())return SessionResult::NotReady;
  if(frame.from!=peer_ || frame.to!=local_)return SessionResult::PeerMismatch;
  if(!frame.id || !out || frame.size<=12 || frame.size>239 || cap<frame.size-12)return SessionResult::InvalidArgument;
  Scalar scalar;if(!load(scalar.bytes))return SessionResult::KeyUnavailable;
  const auto result=crypto_.decryptClassified(scalar.bytes,peerPublic_,peer_,frame.id,
      frame.bytes,frame.size,out,cap,len);
  if(result==experimental::DecryptResult::Ok)return SessionResult::Ok;
  if(result==experimental::DecryptResult::AuthenticationFailed)return SessionResult::AuthenticationFailed;
  return SessionResult::CryptoFailure;
}
bool PkiSession::attachProof(uint32_t request,const uint8_t *routing,size_t n,uint8_t *out,size_t cap,size_t &len) {
  len=0;if(!ready()||!request)return false;
  Scalar scalar;if(!load(scalar.bytes))return false;
  experimental::PkiAck ack(crypto_);
  return ack.attach(scalar.bytes,peerPublic_,{true,local_,peer_,request},routing,n,out,cap,len);
}
experimental::AckProofVerdict PkiSession::verifyProof(uint32_t from,uint32_t to,uint32_t request,
  const uint8_t *routing,size_t n,uint8_t *out,size_t cap,size_t &len) {
  len=0;
  // Correlate before touching a scalar. Pending/expiry is the endpoint owner's job.
  if(!ready()||from!=peer_||to!=local_||!request)return experimental::AckProofVerdict::ContextMismatch;
  Scalar scalar;if(!load(scalar.bytes))return experimental::AckProofVerdict::NoTrustedKey;
  experimental::PkiAck ack(crypto_);
  return ack.verify(scalar.bytes,peerPublic_,{true,peer_,local_,request},{true,from,to,request},routing,n,out,cap,len);
}
}
