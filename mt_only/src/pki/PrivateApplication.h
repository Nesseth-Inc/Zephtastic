// SPDX-License-Identifier: GPL-3.0-only
// PrivateApplication.h
// Outcome m73r2-private-app-header-20261022, attempt 1. Bounded admission
// for private-channel Data envelopes: the TEXT_MESSAGE_APP (port 1) text
// predicate from PkiRuntime::text plus the TELEMETRY_APP (port 67) telemetry
// query request/response via mt::TelemetryQuery and canonical Position
// requests/responses. Endpoint correlation gates private response delivery.
// No crypto, peer/key,
// replay, state, nonce, storage, I/O, allocation, or trust logic.
#pragma once

#include <cstddef>
#include <cstdint>

#include "PkiIdentity.h"
#include "MtCodec.h"
#include "meshtastic/mesh.pb.h"
#include "TelemetryQuery.h"
#include "LocalStatsQuery.h"
#include "PositionQuery.h"
#include "NeighborQuery.h"
#include "RemoteTelemetryQuery.h"
#include "PrivateOpaqueApplication.h"
#if defined(CONFIG_MT_TAK_TRACKER)
#include "../modules/TakPliReplyValidation.h"
#endif

namespace mt {
namespace pki {

struct PrivateApplication {
  static bool keyVerificationPayload(const uint8_t *payload,size_t length) {
    if(!payload||!length||length>meshtastic_KeyVerification_size)return false;
    meshtastic_KeyVerification message{};
    auto in=pb_istream_from_buffer(payload,length);
    if(!pb_decode(&in,meshtastic_KeyVerification_fields,&message)||in.bytes_left||
       !message.nonce||message.hash1.size!=32||
       (message.hash2.size!=0&&message.hash2.size!=32))return false;
    uint8_t expected[meshtastic_KeyVerification_size]{};
    auto out=pb_ostream_from_buffer(expected,sizeof(expected));
    const bool canonical=pb_encode(&out,meshtastic_KeyVerification_fields,&message)&&
      out.bytes_written==length&&!memcmp(expected,payload,length);
    bench::wipeKeyBuffer(expected,sizeof(expected));bench::wipeKeyBuffer(&message,sizeof(message));
    return canonical;
  }
  // This protocol is admitted through a dedicated verifier only. It is not a
  // general private application port and does not enter ordinary owner events.
  static bool keyVerification(const uint8_t *data,size_t n) {
    if(!data||!n||n>meshtastic_Data_size)return false;
    meshtastic_Data envelope{};
    auto in=pb_istream_from_buffer(data,n);
    if(!pb_decode(&in,meshtastic_Data_fields,&envelope)||in.bytes_left||
       envelope.portnum!=meshtastic_PortNum_KEY_VERIFICATION_APP||!envelope.has_bitfield||
       envelope.dest||envelope.source||envelope.xeddsa_signature.size||
       envelope.payload.size>meshtastic_KeyVerification_size)return false;
    if(!keyVerificationPayload(envelope.payload.bytes,envelope.payload.size))return false;
    uint8_t expected[meshtastic_Data_size]{};
    auto dOut=pb_ostream_from_buffer(expected,sizeof(expected));
    const bool canonical=pb_encode(&dOut,meshtastic_Data_fields,&envelope)&&
      dOut.bytes_written==n&&!memcmp(expected,data,n);
    bench::wipeKeyBuffer(expected,sizeof(expected));
    bench::wipeKeyBuffer(&envelope,sizeof(envelope));
    return canonical;
  }
  static bool allowed(const uint8_t *data, size_t n) {
    if(PrivateOpaqueApplication::allowed(data,n))return true;
    struct Wipe {void *p;size_t n;~Wipe(){eraseIdentityBytes(p,n);}};
    bench::RouteMetadata m;Wipe mw{&m,sizeof(m)};
    if(!data||!n||n>227||!bench::MtCodec::decodeData(data,n,m)) return false;
#if defined(CONFIG_MT_TAK_TRACKER)
    if(m.portnum==meshtastic_PortNum_ATAK_PLUGIN_V2&&m.request_id) {
      uint8_t out[227]{};Wipe ow{out,sizeof(out)};size_t size=0;
      return mt::tak_pli_reply::response(m,out,sizeof(out),size)&&size==n&&!memcmp(out,data,n);
    }
#endif
    if(m.portnum==1)
      return m.hasBitfield&&!m.wantResponse&&!(m.bitfield&2)&&!m.request_id&&!m.signatureLength;
    if(m.portnum==meshtastic_PortNum_POSITION_APP) {
      constexpr uint8_t query[]={8,3,24,1,72,2};
      if(m.wantResponse)return n==sizeof(query)&&!memcmp(data,query,n);
      uint8_t out[233]{};Wipe ow{out,sizeof(out)};size_t size=0;
      return mt::PositionQuery::response(m,out,sizeof(out),size)&&size==n&&!memcmp(out,data,n);
    }
    if(m.portnum==meshtastic_PortNum_NEIGHBORINFO_APP) {
      uint8_t out[227]{};Wipe ow{out,sizeof(out)};size_t size=0;
      const bool valid=m.wantResponse?
        (mt::NeighborQuery::requestShape(m)&&mt::NeighborQuery::request(out,sizeof(out),size)):
        mt::NeighborQuery::response(m,out,sizeof(out),size);
      return valid&&size==n&&!memcmp(out,data,n);
    }
    if(m.portnum==67) {
      if(!m.hasBitfield||m.reply_id||m.emoji||m.signatureLength||(m.bitfield&~3u)) return false;
      uint8_t out[233]{};Wipe ow{out,sizeof(out)};size_t size=0;
      if(m.wantResponse&&(m.bitfield&2u)&&!m.request_id) {
        if(!mt::RemoteTelemetryQuery::requestVariant(m.payload,m.payloadLength))return false;
        meshtastic_Data canonical{};Wipe cw{&canonical,sizeof(canonical)};
        canonical.portnum=meshtastic_PortNum_TELEMETRY_APP;canonical.want_response=true;
        canonical.has_bitfield=true;canonical.bitfield=m.bitfield;
        canonical.payload.size=m.payloadLength;memcpy(canonical.payload.bytes,m.payload,m.payloadLength);
        auto encoded=pb_ostream_from_buffer(out,sizeof(out));
        return pb_encode(&encoded,meshtastic_Data_fields,&canonical)&&encoded.bytes_written==n&&!memcmp(out,data,n);
      }
      if(!m.wantResponse&&!(m.bitfield&2u)&&m.request_id)
        return mt::RemoteTelemetryQuery::response(m,0,out,sizeof(out),size)&&size==n&&!memcmp(out,data,n);
      return false;
    }
    return false;
  }
};

}  // namespace pki
}  // namespace mt
