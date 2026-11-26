// SPDX-License-Identifier: GPL-3.0-only
#include "KeyVerification.h"
#include "stock/verification/KeyVerificationModule.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pb_decode.h>
#include <pb_encode.h>

namespace mt::pki::verification {

bool KeyVerification::equal(const uint8_t *a,const uint8_t *b,size_t length) {
  if(!a||!b)return false;
  uint8_t difference=0;
  for(size_t i=0;i<length;++i)difference|=uint8_t(a[i]^b[i]);
  return difference==0;
}
void KeyVerification::wipe(void *value,size_t length) {
  volatile uint8_t *bytes=static_cast<volatile uint8_t *>(value);
  for(size_t i=0;i<length;++i)bytes[i]=0;
}
bool KeyVerification::snapshotLocal() {
  uint32_t node=0;uint8_t key[32]{};
  if(!port_.localIdentity(node,key)||node<=1||node>=0xfffffffeu) {
    wipe(key,sizeof(key));return false;
  }
  uint8_t nonzero=0;for(uint8_t byte:key)nonzero|=byte;
  if(!nonzero){wipe(key,sizeof(key));return false;}
  localNode_=node;memcpy(localKey_,key,sizeof(localKey_));wipe(key,sizeof(key));return true;
}
bool KeyVerification::resolvePeer(uint32_t peer,const uint8_t *candidate,bool candidatePresent) {
  if(peer<=1||peer>=0xfffffffeu||peer==localNode_)return false;
  uint8_t pinned[32]{};
  const bool hasPinned=port_.peerPublicKey(peer,pinned);
  if(candidatePresent&&(!candidate||!equal(candidate,pinned,32))&&hasPinned) {
    wipe(pinned,sizeof(pinned));return false;
  }
  if(hasPinned) {
    memcpy(peerKey_,pinned,32);hasPeerKey_=true;peerKeyWasPinned_=true;
    if(candidatePresent&&!equal(candidate,peerKey_,32)){wipe(pinned,sizeof(pinned));return false;}
    wipe(pinned,sizeof(pinned));return true;
  }
  wipe(pinned,sizeof(pinned));
  if(!candidatePresent||!candidate)return false;
  uint8_t any=0;for(size_t i=0;i<32;++i)any|=candidate[i];
  if(!any)return false;
  memcpy(peerKey_,candidate,32);hasPeerKey_=true;peerKeyWasPinned_=false;
  if(port_.stagePendingCandidate(peer,peerKey_)) {
    wipe(peerKey_,sizeof(peerKey_));hasPeerKey_=false;return false;
  }
  return true;
}
bool KeyVerification::deriveHash1(uint32_t numberValue,uint8_t out[32])const {
  if(!out||!hasPeerKey_)return false;
  uint8_t material[84];
  const uint32_t nodeA=remoteInitiated_?peer_:localNode_;
  const uint32_t nodeB=remoteInitiated_?localNode_:peer_;
  const uint8_t *keyA=remoteInitiated_?peerKey_:localKey_;
  const uint8_t *keyB=remoteInitiated_?localKey_:peerKey_;
  stock::verification::securityNumberTranscript(material,numberValue,nonce_,nodeA,nodeB,keyA,keyB);
  const bool ok=port_.sha256(material,sizeof(material),out);
  wipe(material,sizeof(material));return ok;
}
bool KeyVerification::deriveHash2(const uint8_t hash1[32],uint8_t out[32])const {
  if(!hash1||!out)return false;
  uint8_t material[40];
  stock::verification::challengeTranscript(material,nonce_,hash1);
  const bool ok=port_.sha256(material,sizeof(material),out);
  wipe(material,sizeof(material));return ok;
}
void KeyVerification::notifyNumberRequest() {
  meshtastic_ClientNotification event{};
  event.level=meshtastic_LogRecord_Level_WARNING;
  snprintf(event.message,sizeof(event.message),"Enter Security Number for Key Verification");
  event.which_payload_variant=meshtastic_ClientNotification_key_verification_number_request_tag;
  auto &request=event.payload_variant.key_verification_number_request;
  request.nonce=nonce_;
  (void)port_.peerLongName(peer_,request.remote_longname,sizeof(request.remote_longname));
  request.remote_longname[sizeof(request.remote_longname)-1]='\0';
  port_.notify(event);wipe(&event,sizeof(event));
}
void KeyVerification::notifyNumberInform() {
  meshtastic_ClientNotification event{};
  event.level=meshtastic_LogRecord_Level_WARNING;
  snprintf(event.message,sizeof(event.message),"Incoming Key Verification. Security Number %03u %03u",
           unsigned(securityNumber_/1000),unsigned(securityNumber_%1000));
  event.which_payload_variant=meshtastic_ClientNotification_key_verification_number_inform_tag;
  auto &inform=event.payload_variant.key_verification_number_inform;
  inform.nonce=nonce_;inform.security_number=securityNumber_;
  (void)port_.peerLongName(peer_,inform.remote_longname,sizeof(inform.remote_longname));
  inform.remote_longname[sizeof(inform.remote_longname)-1]='\0';
  port_.notify(event);wipe(&event,sizeof(event));
}
void KeyVerification::notifyFinal(bool sender) {
  char code[10]{};
  stock::verification::generateVerificationCode(hash1_,code);
  meshtastic_ClientNotification event{};
  event.level=meshtastic_LogRecord_Level_WARNING;
  snprintf(event.message,sizeof(event.message),"Final confirmation for %s manual key verification %.9s",
           sender?"outgoing":"incoming",code);
  event.which_payload_variant=meshtastic_ClientNotification_key_verification_final_tag;
  auto &final=event.payload_variant.key_verification_final;
  final.nonce=nonce_;final.isSender=sender;
  memcpy(final.verification_characters,code,sizeof(code));
  (void)port_.peerLongName(peer_,final.remote_longname,sizeof(final.remote_longname));
  final.remote_longname[sizeof(final.remote_longname)-1]='\0';
  port_.notify(event);wipe(code,sizeof(code));wipe(&event,sizeof(event));
}
void KeyVerification::clearSession(bool recordRemoteEnd,uint32_t now) {
  if(hasPeerKey_&&!peerKeyWasPinned_)
    port_.clearPendingCandidate(peer_,peerKey_);
  else if(hasPeerKey_)
    port_.releasePeerSession(peer_,peerKey_,true);
  if(recordRemoteEnd&&remoteInitiated_){lastRemoteEndMs_=now;hasRemoteEnd_=true;}
  state_=State::Idle;peer_=0;localNode_=0;startMs_=0;lastProgressMs_=0;
  securityNumber_=0;nonce_=0;hasPeerKey_=false;peerKeyWasPinned_=false;remoteInitiated_=false;
  wipe(localKey_,sizeof(localKey_));wipe(peerKey_,sizeof(peerKey_));
  wipe(hash1_,sizeof(hash1_));wipe(hash2_,sizeof(hash2_));
}
void KeyVerification::update(uint32_t now) {
  if(state_==State::Idle)return;
  if(uint32_t(now-startMs_)>=MaxSessionMs||
     uint32_t(now-lastProgressMs_)>=IdleTimeoutMs)
    clearSession(true,now);
}
int KeyVerification::sendInitial(uint32_t now) {
  if(state_!=State::Idle)return -EBUSY;
  if(!snapshotLocal()){clearSession(false,now);return -ENOTSUP;}
  uint8_t nonceBytes[8]{};
  if(!port_.random(nonceBytes,sizeof(nonceBytes))) {
    wipe(nonceBytes,sizeof(nonceBytes));clearSession(false,now);return -EIO;
  }
  for(unsigned i=0;i<8;++i)nonce_|=uint64_t(nonceBytes[i])<<(8*i);
  wipe(nonceBytes,sizeof(nonceBytes));
  if(!nonce_){clearSession(false,now);return -EIO;}
  uint8_t known[32]{};
  const bool knownKey=port_.peerPublicKey(peer_,known);
  if(knownKey){memcpy(peerKey_,known,32);hasPeerKey_=true;peerKeyWasPinned_=true;}
  wipe(known,sizeof(known));
  meshtastic_KeyVerification message{};
  message.nonce=nonce_;message.hash1.size=32;memcpy(message.hash1.bytes,localKey_,32);
  const int result=port_.send(peer_,0,message,knownKey,knownKey?peerKey_:nullptr,
                              knownKey?32:0,true,now);
  if(result){clearSession(false,now);return result;}
  startMs_=lastProgressMs_=now;state_=State::SenderStarted;
  return 0;
}
int KeyVerification::acceptInitial(uint32_t from,uint32_t packetId,bool pki,
                                   const uint8_t *envelopeKey,size_t envelopeKeyLength,
                                   const meshtastic_KeyVerification &message,uint32_t now) {
  if(state_!=State::Idle)return -EBUSY;
  if(hasRemoteEnd_&&uint32_t(now-lastRemoteEndMs_)<RemoteCooldownMs)return -EAGAIN;
  if(!message.nonce||message.hash2.size||message.hash1.size!=32)return -EINVAL;
  uint8_t pinned[32]{};
  const bool alreadyPinned=port_.peerPublicKey(from,pinned);
  const bool pinnedEnvelopeOk=!alreadyPinned||
    (pki&&envelopeKeyLength==32&&envelopeKey&&equal(pinned,envelopeKey,32));
  wipe(pinned,sizeof(pinned));
  if(!pinnedEnvelopeOk)return -EKEYREJECTED;
  const uint8_t *candidate=nullptr;bool haveCandidate=false;
  if(pki) {
    if(envelopeKeyLength!=32||!envelopeKey)return -EINVAL;
    candidate=envelopeKey;haveCandidate=true;
    if(message.hash1.size==32&&!equal(message.hash1.bytes,envelopeKey,32))return -EKEYREJECTED;
  } else if(message.hash1.size==32) {
    candidate=message.hash1.bytes;haveCandidate=true;
  }
  if(!snapshotLocal())return -ENOTSUP;
  peer_=from;nonce_=message.nonce;remoteInitiated_=true;
  if(!resolvePeer(from,candidate,haveCandidate)){clearSession(false,now);return -EKEYREJECTED;}
  uint8_t entropy[4]{};
  if(!port_.random(entropy,sizeof(entropy))){wipe(entropy,sizeof(entropy));clearSession(false,now);return -EIO;}
  securityNumber_=uint32_t(entropy[0])|(uint32_t(entropy[1])<<8)|
    (uint32_t(entropy[2])<<16)|(uint32_t(entropy[3])<<24);
  wipe(entropy,sizeof(entropy));
  securityNumber_=(securityNumber_%999999u)+1u;
  if(!deriveHash1(securityNumber_,hash1_)||!deriveHash2(hash1_,hash2_)) {
    clearSession(false,now);return -EIO;
  }
  meshtastic_KeyVerification response{};
  response.nonce=nonce_;response.hash1.size=32;memcpy(response.hash1.bytes,localKey_,32);
  response.hash2.size=32;memcpy(response.hash2.bytes,hash2_,32);
  const int result=port_.send(peer_,packetId,response,peerKeyWasPinned_,peerKeyWasPinned_?peerKey_:nullptr,
                              peerKeyWasPinned_?32:0,false,now);
  if(result){clearSession(false,now);return result;}
  state_=State::ReceiverAwaitingHash1;startMs_=lastProgressMs_=now;
  notifyNumberInform();return 0;
}
int KeyVerification::acceptResponse(uint32_t from,bool pki,const uint8_t *envelopeKey,
                                    size_t envelopeKeyLength,
                                    const meshtastic_KeyVerification &message,uint32_t now) {
  if(state_!=State::SenderStarted||from!=peer_||message.nonce!=nonce_||
     message.hash1.size!=32||message.hash2.size!=32)return -EINVAL;
  if(peerKeyWasPinned_&&!pki)return -EKEYREJECTED;
  if(pki&&(envelopeKeyLength!=32||!envelopeKey||
           !equal(envelopeKey,message.hash1.bytes,32)))return -EKEYREJECTED;
  if(!resolvePeer(peer_,message.hash1.bytes,true)){clearSession(false,now);return -EKEYREJECTED;}
  memcpy(hash2_,message.hash2.bytes,32);
  state_=State::SenderAwaitingNumber;lastProgressMs_=now;
  notifyNumberRequest();return 0;
}
int KeyVerification::acceptFinal(uint32_t from,bool pki,const uint8_t *envelopeKey,
                                 size_t envelopeKeyLength,
                                 const meshtastic_KeyVerification &message,uint32_t now) {
  if(state_!=State::ReceiverAwaitingHash1||from!=peer_||!pki||
     envelopeKeyLength!=32||!envelopeKey||!equal(envelopeKey,peerKey_,32)||
     message.nonce!=nonce_||message.hash1.size!=32||message.hash2.size!=0)return -EINVAL;
  if(!equal(message.hash1.bytes,hash1_,32))return -EKEYREJECTED;
  state_=State::ReceiverAwaitingConfirmation;lastProgressMs_=now;
  notifyFinal(false);return 0;
}
int KeyVerification::provideNumber(uint32_t value,uint32_t now) {
  if(state_!=State::SenderAwaitingNumber)return -EINVAL;
  if(value<1||value>999999)return -EINVAL;
  uint8_t candidateHash[32]{},candidateHash2[32]{};
  const bool derived=deriveHash1(value,candidateHash)&&deriveHash2(candidateHash,candidateHash2);
  const bool matches=derived&&equal(candidateHash2,hash2_,32);
  wipe(candidateHash2,sizeof(candidateHash2));
  if(!matches){wipe(candidateHash,sizeof(candidateHash));return -EACCES;}
  memcpy(hash1_,candidateHash,32);wipe(candidateHash,sizeof(candidateHash));
  securityNumber_=value;
  const auto message=stock::verification::numberProof(nonce_,hash1_);
  const int result=port_.send(peer_,0,message,true,peerKey_,32,true,now);
  if(result)return result;
  state_=State::SenderAwaitingConfirmation;lastProgressMs_=now;
  notifyFinal(true);return 0;
}
int KeyVerification::approve(uint64_t nonceValue,uint32_t now) {
  if((state_!=State::SenderAwaitingConfirmation&&state_!=State::ReceiverAwaitingConfirmation)||
     !nonceValue||nonceValue!=nonce_||!hasPeerKey_)return -EINVAL;
  const int result=port_.commitVerifiedPeer(peer_,peerKey_);
  if(result)return result;
  clearSession(true,now);return 0;
}
void KeyVerification::cancel(uint32_t now) {
  if(state_!=State::Idle)clearSession(true,now);
}
int KeyVerification::handleAdmin(const meshtastic_KeyVerificationAdmin &request,uint32_t now) {
  update(now);
  const uint32_t type=uint32_t(request.message_type);
  if(type==meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION) {
    if(request.remote_nodenum<=1||request.remote_nodenum>=0xfffffffeu||
       request.has_security_number||request.nonce)return -EINVAL;
    if(state_!=State::Idle)return -EBUSY;
    peer_=request.remote_nodenum;
    return sendInitial(now);
  }
  if(type==meshtastic_KeyVerificationAdmin_MessageType_PROVIDE_SECURITY_NUMBER) {
    if(!request.has_security_number||!request.nonce||request.remote_nodenum)return -EINVAL;
    if(request.nonce!=nonce_)return -ESTALE;
    return provideNumber(request.security_number,now);
  }
  if(type==meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY) {
    if(!request.nonce||request.remote_nodenum||request.has_security_number)return -EINVAL;
    return approve(request.nonce,now);
  }
  if(type==meshtastic_KeyVerificationAdmin_MessageType_DO_NOT_VERIFY) {
    if(request.remote_nodenum||request.has_security_number)return -EINVAL;
    if(request.nonce&&request.nonce!=nonce_)return -ESTALE;
    cancel(now);return 0;
  }
  return -EINVAL;
}
int KeyVerification::receive(uint32_t from,uint32_t packetId,bool pki,
                             const uint8_t *envelopeKey,size_t envelopeKeyLength,
                             const uint8_t *payload,size_t payloadLength,uint32_t now) {
  update(now);
  if(from<=1||from>=0xfffffffeu||!packetId||!payload||
     payloadLength>meshtastic_KeyVerification_size||
     (pki&&(envelopeKeyLength!=32||!envelopeKey)))return -EINVAL;
  meshtastic_KeyVerification message{};
  auto input=pb_istream_from_buffer(payload,payloadLength);
  if(!pb_decode(&input,meshtastic_KeyVerification_fields,&message)||input.bytes_left)return -EINVAL;
  uint8_t canonical[meshtastic_KeyVerification_size]{};
  auto output=pb_ostream_from_buffer(canonical,sizeof(canonical));
  const bool canonicalWire=pb_encode(&output,meshtastic_KeyVerification_fields,&message)&&
    output.bytes_written==payloadLength&&!memcmp(canonical,payload,payloadLength);
  wipe(canonical,sizeof(canonical));
  if(!canonicalWire)return -EINVAL;
  if(state_==State::Idle)return acceptInitial(from,packetId,pki,envelopeKey,envelopeKeyLength,message,now);
  if(state_==State::SenderStarted)return acceptResponse(from,pki,envelopeKey,envelopeKeyLength,message,now);
  if(state_==State::ReceiverAwaitingHash1)return acceptFinal(from,pki,envelopeKey,envelopeKeyLength,message,now);
  return -EBUSY;
}
void KeyVerification::poll(uint32_t now){update(now);}

} // namespace mt::pki::verification

