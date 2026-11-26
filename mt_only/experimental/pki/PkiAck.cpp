// SPDX-License-Identifier: GPL-3.0-only
#include "PkiAck.h"
#include "monocypher.h"
#include <pb_decode.h>
#include <string.h>

namespace mt::experimental {
namespace {
bool unicast(uint32_t node){return node!=0 && node!=UINT32_MAX;}
bool shape(const AckEnvelope &c) {
  return c.decodedRouting && unicast(c.from) && unicast(c.to) &&
         c.from!=c.to && c.request!=0;
}
}
ProofFieldResult PkiAck::inspect(const uint8_t *wire,size_t n,ProofField &field) {
  if((!wire && n) || n>MaxRouting)return ProofFieldResult::Malformed;
  pb_istream_t stream=pb_istream_from_buffer(wire,n);
  ProofField parsed{};bool found=false;
  while(stream.bytes_left) {
    const size_t start=n-stream.bytes_left;
    pb_wire_type_t type;uint32_t tag;bool eof=false;
    if(!pb_decode_tag(&stream,&type,&tag,&eof) || tag==0)return ProofFieldResult::Malformed;
    if(tag==4) {
      // Stock ignores wrong-type field 4. This experiment intentionally rejects
      // it: there must be exactly one unambiguous proof field and no shadow one.
      if(found || type!=PB_WT_STRING)return ProofFieldResult::Malformed;
      pb_istream_t sub;
      if(!pb_make_string_substream(&stream,&sub))return ProofFieldResult::Malformed;
      if(sub.bytes_left!=8 || !pb_read(&sub,parsed.proof,8) ||
         !pb_close_string_substream(&stream,&sub))return ProofFieldResult::Malformed;
      parsed.start=start;parsed.length=n-stream.bytes_left-start;found=true;
    }else if(!pb_skip_field(&stream,type))return ProofFieldResult::Malformed;
  }
  if(!found)return ProofFieldResult::Absent;
  field=parsed;return ProofFieldResult::Present;
}
bool PkiAck::attach(const uint8_t secret[32],const uint8_t peer[32],
                    const AckEnvelope &context,const uint8_t *routing,size_t n,
                    uint8_t *out,size_t cap,size_t &len) {
  len=0;
  if(!shape(context) || !secret || !peer || !out || n>MaxRouting-FieldSize ||
     cap<n+FieldSize)return false;
  ProofField field{};
  if(inspect(routing,n,field)!=ProofFieldResult::Absent)return false;
  uint8_t tmp[MaxRouting]{};
  const bool ok=crypto_.proof(secret,peer,context.from,context.to,context.request,
                              routing,n,tmp+n+2);
  if(ok) {
    if(n)memcpy(tmp,routing,n);
    tmp[n]=0x22;tmp[n+1]=8;
    memcpy(out,tmp,n+FieldSize);len=n+FieldSize;
  }
  crypto_wipe(tmp,sizeof(tmp));return ok;
}
AckProofVerdict PkiAck::verify(const uint8_t secret[32],const uint8_t peer[32],
                             const PendingAck &pending,const AckEnvelope &received,
                             const uint8_t *wire,size_t n,uint8_t *routing,
                             size_t cap,size_t &len) {
  len=0;
  if(!pending.active || !shape(received) || received.from!=pending.peer ||
     received.to!=pending.local || received.request!=pending.request)
    return AckProofVerdict::ContextMismatch;
  if(!peer)return AckProofVerdict::NoTrustedKey;
  if(!secret || !routing)return AckProofVerdict::InvalidInput;
  ProofField field{};const auto parsed=inspect(wire,n,field);
  if(parsed==ProofFieldResult::Malformed)return AckProofVerdict::Malformed;
  if(parsed==ProofFieldResult::Absent)return AckProofVerdict::Absent;
  const size_t size=n-field.length;
  if(cap<size)return AckProofVerdict::InvalidInput;
  uint8_t tmp[MaxRouting]{};
  if(field.start)memcpy(tmp,wire,field.start);
  const size_t tail=n-field.start-field.length;
  if(tail)memcpy(tmp+field.start,wire+field.start+field.length,tail);
  const bool ok=crypto_.verify(secret,peer,received.from,received.to,received.request,
                               tmp,size,field.proof);
  if(ok){if(size)memcpy(routing,tmp,size);len=size;}
  crypto_wipe(tmp,sizeof(tmp));crypto_wipe(field.proof,sizeof(field.proof));
  return ok?AckProofVerdict::ValidProof:AckProofVerdict::InvalidProof;
}
}
