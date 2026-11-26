// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt::pki {
enum class ReplayCheck {Fresh,Seen,UnknownPeer,WrongKey,NotReady,Full,Invalid};
// Injected trusted authority; no advertisement/NodeCache promotion interface.
// Every binding belongs to one stable local public-key/node domain. Caller
// serializes all operations with identity/trust mutations and service storage.
struct PeerReplayAuthority {
  virtual ~PeerReplayAuthority()=default;
  virtual bool boundTo(const uint8_t localPublic[32],uint32_t local)const=0;
  virtual bool lookup(uint32_t peer,uint8_t outPublic[32])const=0;
  virtual bool active(uint32_t peer,const uint8_t publicKey[32])const=0;
  virtual ReplayCheck seen(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId)const=0;
  // AUTHENTICATED receive only. Success durably records the ID before plaintext
  // delivery. A true result does not itself prove authentication. Duplicate ACK
  // handling must authenticate the actual received bytes independently of seen().
  virtual bool commitAuthenticated(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId)=0;
  // Explicit local Remove/Ignore only: persist irreversible revocation before
  // changing visible cache state. Default refuses for authorities without durable
  // revocation. A true result never authorizes erasing nonce/replay history.
  // Read-only durable tombstone evidence, not key lookup or trust admission.
  virtual bool revoked(uint32_t)const{return false;}
  virtual bool revokeTrustedPeer(uint32_t,const uint8_t[32]){return false;}
};
}
