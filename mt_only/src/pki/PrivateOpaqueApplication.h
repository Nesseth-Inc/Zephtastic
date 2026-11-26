// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiIdentity.h"
#include "PublicApplication.h"
#include <string.h>
namespace mt::pki {
// Canonical opaque Data only. This installs no application module and never
// writes shared public status/position caches or synthesizes a replay packet.
struct PrivateOpaqueApplication {
  static bool allowed(const uint8_t *data,size_t n) {
    struct Wipe {void *p;size_t n;~Wipe(){eraseIdentityBytes(p,n);}};
    if(!data||!n||n>227)return false;
    bench::RouteMetadata m{};Wipe mw{&m,sizeof(m)};
    if(!bench::MtCodec::decodeData(data,n,m)||!PublicApplication::allowed(m.portnum)||
       !m.hasBitfield||m.bitfield>1||m.wantResponse||m.request_id||m.signatureLength||m.payloadLength>233)return false;
    meshtastic_Data canonical{};Wipe cw{&canonical,sizeof(canonical)};
    canonical.portnum=static_cast<meshtastic_PortNum>(m.portnum);
    canonical.payload.size=m.payloadLength;if(m.payloadLength)memcpy(canonical.payload.bytes,m.payload,m.payloadLength);
    canonical.reply_id=m.reply_id;canonical.emoji=m.emoji;canonical.has_bitfield=true;canonical.bitfield=m.bitfield;
    uint8_t out[227]{};Wipe ow{out,sizeof(out)};auto encoded=pb_ostream_from_buffer(out,sizeof(out));
    // Zeroed omitted routing fields also reject explicit dest/source bytes.
    // Whole byte equality rejects unknown/duplicate/reordered/nonminimal wire.
    return pb_encode(&encoded,meshtastic_Data_fields,&canonical)&&encoded.bytes_written==n&&!memcmp(out,data,n);
  }
};
}
