// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiIdentity.h"
#include "meshtastic/mesh.pb.h"

namespace mt::pki {
struct SigningEntropy {
  virtual ~SigningEntropy()=default;
  // Supply exactly 32 fresh CSPRNG bytes, or fail. No weak/default fallback.
  // The implementation must not retain the pointer or reenter this operation.
  virtual bool hedge(uint8_t out[32])=0;
};
struct SignedData {
  static constexpr size_t Capacity=239;
  uint32_t from=0,to=0,id=0;
  size_t size=0;
  uint8_t bytes[Capacity]{};
};
struct NodeInfoProfile {
  char longName[25]{},shortName[5]{};
  uint8_t role=1; // Bounded normal CLIENT/CLIENT_MUTE/ROUTER.
  uint8_t hardwareModel=0; // Meshtastic hardware model of a supported board.
  bool ownerUnmessagable=false;
  bool licensed=false;
};
struct SigningEnvelope {
  uint32_t requestId=0,replyId=0,emoji=0;
  bool wantResponse=false,hasBitfield=false;
  uint8_t bitfield=0;
};
enum class SigningResult {
  Ok,InvalidArgument,NotReady,KeyUnavailable,EntropyUnavailable,TooLarge,CryptoFailure
};
// Pure producer. No packet-ID allocation/reservation, radio, queue, persistent
// state, capability advertisement or peer trust. Serialize with key mutations.
// The caller supplies a durably reserved ID before channel encryption, binds
// final from/to/id unchanged, and caches the exact result for retries.
struct PkiSigner {
private:
  static SigningResult signPrepared(KeyProvider &,KeyRef,const IdentityBinding &,SigningEntropy &,
                                     uint32_t id,uint32_t to,const meshtastic_Data &,SignedData &out,bool attachSignature=true);
public:
  // Verifies active binding and actual borrowed scalar, without consuming
  // entropy or retaining the key. A successful check is not a future lease.
  static bool ready(KeyProvider &,KeyRef,const IdentityBinding &);
  // Bounded text/NodeInfo/telemetry, canonical Position broadcast, plus
  // Status/RangeTest/NeighborInfo module broadcasts. New module ports reject
  // unicast/query envelopes. Data's request/reply/emoji,
  // bitfield presence/value and want_response are covered exactly as stock.
  // Nonzero legacy dest/source and preexisting signatures are rejected. These
  // fields are outside stock's signed envelope and must not imply provenance.
  // Out is unchanged on every failure; borrowed scalar and scratch are wiped.
  static SigningResult sign(KeyProvider &,KeyRef,const IdentityBinding &,SigningEntropy &,
                            uint32_t id,uint32_t to,const meshtastic_Data &,SignedData &out,bool attachSignature=true);
  // Canonical non-module application broadcasts accepted by
  // PublicApplication::canonical. Active moduleBroadcastPort values stay on
  // their dedicated typed producer. Decodes and validates the complete wire
  // body before signing; caller never supplies route identity, destination,
  // packet ID, or signature bytes.
  static SigningResult signPublicApplicationBroadcast(KeyProvider &,KeyRef,const IdentityBinding &,
    SigningEntropy &,uint32_t id,const uint8_t *data,size_t size,SignedData &out,bool attachSignature=true);
  // Licensed plaintext unicast only. Requires a legal unicast destination and
  // a canonical supported application/control shape; it never authorizes PKI
  // fallback or accepts an input signature supplied by the caller.
  static SigningResult signLicensedUnicast(KeyProvider &,KeyRef,const IdentityBinding &,SigningEntropy &,
                                           uint32_t id,uint32_t to,const meshtastic_Data &,SignedData &out,bool attachSignature=true);
  // Construct authoritative User.id/public_key from the supplied identity.
  // Owner role/license metadata is explicit; no MAC claim. Envelope preserved.
  // Bootstrap encoded Data is additionally bounded to the private path's 227B.
  // This establishes possession/binding only, never an owner's human identity.
  static SigningResult bootstrap(KeyProvider &,KeyRef,const IdentityBinding &,SigningEntropy &,
                                 uint32_t id,uint32_t to,const NodeInfoProfile &,
                                 const SigningEnvelope &,SignedData &out,bool attachSignature=true);
};
}
