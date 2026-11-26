// SPDX-License-Identifier: GPL-3.0-only
#include "KeyVerificationService.h"
#include "monocypher.h"
#include <pb_encode.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace mt::pki::verification {
namespace {
struct Wipe {void *p;size_t n;~Wipe(){crypto_wipe(p,n);}};
uint32_t nextSeed(uint32_t value) {
  value+=0x9e3779b9u;value^=value>>16;value*=0x7feb352du;
  value^=value>>15;value*=0x846ca68bu;value^=value>>16;
  return value?value:0x6d2b79f5u;
}
bool transientAdmissionFailure(int result) {
  return result==-ENOBUFS||result==-EBUSY||result==-EAGAIN;
}
}

bool KeyVerificationService::Adapter::localIdentity(uint32_t &node,uint8_t key[32])const {
  return owner_.service_.keyVerificationIdentity(node,key);
}
bool KeyVerificationService::Adapter::peerPublicKey(uint32_t node,uint8_t key[32])const {
  return owner_.service_.keyVerificationPeerKey(node,key);
}
bool KeyVerificationService::Adapter::random(uint8_t *out,size_t length) {
  return owner_.service_.keyVerificationRandom(out,length);
}
bool KeyVerificationService::Adapter::sha256(const uint8_t *data,size_t length,uint8_t out[32]) {
  return owner_.service_.keyVerificationSha256(data,length,out);
}
int KeyVerificationService::Adapter::stagePendingCandidate(uint32_t node,const uint8_t key[32]) {
  return owner_.service_.keyVerificationStageCandidate(node,key);
}
void KeyVerificationService::Adapter::clearPendingCandidate(uint32_t node,const uint8_t key[32]) {
  owner_.service_.keyVerificationClearCandidate(node,key);
}
void KeyVerificationService::Adapter::releasePeerSession(uint32_t node,const uint8_t key[32],bool pinned) {
  if(pinned)owner_.service_.keyVerificationReleasePeer(node,key);
}
int KeyVerificationService::Adapter::commitVerifiedPeer(uint32_t node,const uint8_t key[32]) {
  if(owner_.service_.config.licensedOperation())return -EPERM;
  return owner_.commit_?owner_.commit_(owner_.commitContext_,node,key):-ENOTSUP;
}
bool KeyVerificationService::Adapter::peerLongName(uint32_t node,char *out,size_t capacity)const {
  return owner_.service_.keyVerificationPeerName(node,out,capacity);
}
void KeyVerificationService::Adapter::notify(const meshtastic_ClientNotification &event) {
  auto &owner=owner_;
  if(owner.notificationCount_==2) {
    crypto_wipe(&owner.notifications_[owner.notificationRead_],sizeof(owner.notifications_[0]));
    owner.notificationRead_=uint8_t((owner.notificationRead_+1u)%2u);
    --owner.notificationCount_;
  }
  const unsigned slot=(owner.notificationRead_+owner.notificationCount_)%2u;
  owner.notifications_[slot]=event;++owner.notificationCount_;
}
int KeyVerificationService::Adapter::send(uint32_t peer,uint32_t replyTo,
    const meshtastic_KeyVerification &message,bool pki,const uint8_t *explicitKey,
    size_t keyLength,bool wantResponse,uint32_t now) {
  uint8_t payload[meshtastic_KeyVerification_size]{};Wipe wipePayload{payload,sizeof(payload)};
  auto pOut=pb_ostream_from_buffer(payload,sizeof(payload));
  if(!pb_encode(&pOut,meshtastic_KeyVerification_fields,&message)||!pOut.bytes_written)return -EINVAL;
  meshtastic_Data envelope{};Wipe wipeEnvelope{&envelope,sizeof(envelope)};
  envelope.portnum=meshtastic_PortNum_KEY_VERIFICATION_APP;
  envelope.has_bitfield=true;envelope.want_response=wantResponse;
  envelope.reply_id=replyTo;envelope.payload.size=pb_size_t(pOut.bytes_written);
  memcpy(envelope.payload.bytes,payload,pOut.bytes_written);
  uint8_t wire[meshtastic_Data_size]{};Wipe wipeWire{wire,sizeof(wire)};
  auto dOut=pb_ostream_from_buffer(wire,sizeof(wire));
  if(!pb_encode(&dOut,meshtastic_Data_fields,&envelope)||!dOut.bytes_written||
     !PrivateApplication::keyVerification(wire,dOut.bytes_written))return -EINVAL;
  uint8_t idBytes[4]{};Wipe wipeId{ idBytes,sizeof(idBytes) };
  if(!owner_.service_.keyVerificationRandom(idBytes,sizeof(idBytes)))return -EIO;
  const uint32_t random=uint32_t(idBytes[0])|(uint32_t(idBytes[1])<<8)|
    (uint32_t(idBytes[2])<<16)|(uint32_t(idBytes[3])<<24);
  if(owner_.pending_.active)return -EBUSY;
  const uint32_t hop=owner_.service_.keyVerificationHopLimit();
  const int result=owner_.service_.submitKeyVerification(owner_.owner_,peer,replyTo,wire,dOut.bytes_written,
    pki,explicitKey,keyLength,hop,true,now,random);
  if(!transientAdmissionFailure(result))return result;
  return owner_.defer(wire,dOut.bytes_written,peer,replyTo,pki,explicitKey,keyLength,
                      hop,random,message.nonce,now)?0:result;
}

int KeyVerificationService::transmit(const PendingSend &pending,uint32_t now) {
  return service_.submitKeyVerification(pending.owner,pending.peer,pending.replyTo,
    pending.data,pending.length,pending.pki,
    pending.pki?pending.explicitKey:nullptr,pending.keyLength,pending.hop,true,
    now,pending.randomSeed);
}
bool KeyVerificationService::defer(const uint8_t *data,size_t length,uint32_t peer,uint32_t replyTo,
    bool pki,const uint8_t *explicitKey,size_t keyLength,uint32_t hop,uint32_t randomSeed,
    uint64_t nonce,uint32_t now) {
  if(pending_.active||!data||!length||length>sizeof(pending_.data)||!nonce||
     keyLength>(pki?sizeof(pending_.explicitKey):0)||(pki&&(!explicitKey||keyLength!=32)))return false;
  pending_={};memcpy(pending_.data,data,length);
  if(pki)memcpy(pending_.explicitKey,explicitKey,sizeof(pending_.explicitKey));
  pending_.length=length;pending_.keyLength=keyLength;pending_.owner=owner_;
  pending_.peer=peer;pending_.replyTo=replyTo;pending_.hop=hop;
  pending_.randomSeed=randomSeed;pending_.queuedAt=now;pending_.lastAttempt=now;
  pending_.nonce=nonce;pending_.pki=pki;pending_.active=true;
  return true;
}
void KeyVerificationService::clearPending() {crypto_wipe(&pending_,sizeof(pending_));}
void KeyVerificationService::notifyPendingFailure() {
  meshtastic_ClientNotification event{};
  event.level=meshtastic_LogRecord_Level_WARNING;
  snprintf(event.message,sizeof(event.message),"Key verification message was not admitted to the radio queue");
  adapter_.notify(event);crypto_wipe(&event,sizeof(event));
}
void KeyVerificationService::retryPending(uint32_t now) {
  if(!pending_.active)return;
  const bool sameSession=core_.state()!=KeyVerification::State::Idle&&core_.nonce()==pending_.nonce;
  if(!pending_.afterSessionApproval&&!sameSession) {clearPending();notifyPendingFailure();return;}
  if(pending_.afterSessionApproval&&uint32_t(now-pending_.queuedAt)>=ApprovedSendLimitMs) {
    clearPending();notifyPendingFailure();return;
  }
  if(uint32_t(now-pending_.lastAttempt)<PendingRetryMs)return;
  pending_.lastAttempt=now;
  const int result=transmit(pending_,now);
  if(!result) {clearPending();return;}
  if(transientAdmissionFailure(result)) {
    // Only packet-ID seed changes across a known-not-admitted retry. The
    // serialized KeyVerification Data/challenge bytes remain immutable.
    pending_.randomSeed=nextSeed(pending_.randomSeed);return;
  }
  clearPending();
  if(sameSession) {core_.cancel(now);owner_=Endpoint::LocalUiOwner;}
  notifyPendingFailure();
}

int KeyVerificationService::admin(uint8_t owner,const meshtastic_KeyVerificationAdmin &request,uint32_t now) {
  if(owner>=LocalClientCount)return -EINVAL;
  const uint32_t type=uint32_t(request.message_type);
  if(service_.config.licensedOperation()){
    if(type==meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION){
      meshtastic_ClientNotification warning{};warning.level=meshtastic_LogRecord_Level_WARNING;
      snprintf(warning.message,sizeof(warning.message),"Key verification is unavailable in licensed (ham) mode");
      adapter_.notify(warning);
    }
    return -EPERM;
  }
  if(pending_.active&&core_.state()==KeyVerification::State::Idle&&
     type==meshtastic_KeyVerificationAdmin_MessageType_INITIATE_VERIFICATION)return -EBUSY;
  if(core_.state()!=KeyVerification::State::Idle&&owner_!=owner) {
    // An RF-initiated session begins unclaimed by a local transport. Permit a
    // local owner to claim it only with a well-shaped, nonce-matching explicit
    // decision in the final-confirmation state, or a nonce-matching cancel.
    // Invalid requests and a second owner's local initiation cannot capture it.
    const bool validClaim=(type==meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY&&
      core_.remoteInitiated()&&core_.state()==KeyVerification::State::ReceiverAwaitingConfirmation&&
      request.nonce&&request.nonce==core_.nonce()&&!request.remote_nodenum&&!request.has_security_number)||
      (type==meshtastic_KeyVerificationAdmin_MessageType_DO_NOT_VERIFY&&
       core_.remoteInitiated()&&request.nonce&&request.nonce==core_.nonce()&&
       !request.remote_nodenum&&!request.has_security_number);
    if(owner_!=Endpoint::LocalUiOwner||!validClaim)return -EBUSY;
    owner_=owner;
  }
  if(core_.state()==KeyVerification::State::Idle)owner_=owner;
  const int result=core_.handleAdmin(request,now);
  if(!result&&type==meshtastic_KeyVerificationAdmin_MessageType_DO_NOT_VERIFY)clearPending();
  if(!result&&type==meshtastic_KeyVerificationAdmin_MessageType_DO_VERIFY&&pending_.active&&
     core_.state()==KeyVerification::State::Idle)pending_.afterSessionApproval=true;
  if(core_.state()==KeyVerification::State::Idle)owner_=Endpoint::LocalUiOwner;
  return result;
}
int KeyVerificationService::receive(const KeyVerificationReceive &event,uint32_t now) {
  if(service_.config.licensedOperation())return -EPERM;
  if(event.from<=1||event.from>=0xfffffffeu||!event.id||!event.size||
     event.size>sizeof(event.payload)||!PrivateApplication::keyVerificationPayload(event.payload,event.size))return -EINVAL;
  if(core_.state()==KeyVerification::State::Idle)owner_=Endpoint::LocalUiOwner;
  const int result=core_.receive(event.from,event.id,event.pki,
    event.pki?event.publicKey:nullptr,event.pki?sizeof(event.publicKey):0,
    event.payload,event.size,now);
  if(core_.state()==KeyVerification::State::Idle)owner_=Endpoint::LocalUiOwner;
  return result;
}
void KeyVerificationService::poll(uint32_t now) {
  if(service_.config.licensedOperation()){
    if(pending_.active)clearPending();
    if(core_.state()!=KeyVerification::State::Idle)core_.cancel(now);
    owner_=Endpoint::LocalUiOwner;return;
  }
  core_.poll(now);
  if(pending_.active)retryPending(now);
  if(core_.state()==KeyVerification::State::Idle)owner_=Endpoint::LocalUiOwner;
}
void KeyVerificationService::disconnect(uint8_t owner,uint32_t now) {
  if(pending_.active&&pending_.owner==owner)clearPending();
  if(owner_==owner&&core_.state()!=KeyVerification::State::Idle)core_.cancel(now);
  if(core_.state()==KeyVerification::State::Idle)owner_=Endpoint::LocalUiOwner;
}
bool KeyVerificationService::takeNotification(meshtastic_ClientNotification &out) {
  out={};if(!notificationCount_)return false;
  out=notifications_[notificationRead_];crypto_wipe(&notifications_[notificationRead_],sizeof(out));
  notificationRead_=uint8_t((notificationRead_+1u)%2u);--notificationCount_;return true;
}

} // namespace mt::pki::verification
