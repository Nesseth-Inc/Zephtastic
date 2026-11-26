// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "MtKeyCommand.h"
#include "RelayCore.h"
#include "../PublicApplication.h"

namespace mt::network {
// Publish-only ordinary channel data. This extends the opaque application
// allowlist with the existing text and common announcement ports. It never
// treats a decoded channel frame as authenticated PKI or an admin request.
class MqttDataPolicy {
public:
  static bool allowed(uint32_t port) {
    return port==1||port==3||port==4||port==67||PublicApplication::allowed(port);
  }
  static bool encode(const bench::RouteMetadata &m,uint8_t *out,size_t capacity,size_t &length) {
    length=0;
    if(!allowed(m.portnum)||!m.hasBitfield||m.bitfield>1||m.wantResponse||m.request_id||
       (m.signatureLength&&(m.signatureLength!=64||!m.signatureVerified))||m.payloadLength>233)return false;
    meshtastic_Data data{};data.portnum=static_cast<meshtastic_PortNum>(m.portnum);
    data.payload.size=m.payloadLength;memcpy(data.payload.bytes,m.payload,m.payloadLength);
    if(m.signatureLength){
      data.xeddsa_signature.size=m.signatureLength;
      memcpy(data.xeddsa_signature.bytes,m.signature,m.signatureLength);
    }
    data.reply_id=m.reply_id;data.emoji=m.emoji;data.has_bitfield=true;data.bitfield=m.bitfield;
    auto output=pb_ostream_from_buffer(out,capacity<239?capacity:239);
    if(!pb_encode(&output,meshtastic_Data_fields,&data))return false;
    length=output.bytes_written;return true;
  }
  static bool canonical(const uint8_t *wire,size_t length,bool signatureVerified=false) {
    if(!wire||!length||length>239)return false;
    bench::RouteMetadata metadata{};
    if(!bench::MtCodec::decodeData(wire,length,metadata))return false;
    metadata.signatureVerified=signatureVerified;
    uint8_t expected[239]{};size_t bytes=0;
    const bool ok=encode(metadata,expected,sizeof(expected),bytes)&&bytes==length&&
      !memcmp(wire,expected,bytes);
    bench::wipeKeyBuffer(expected,sizeof(expected));
    bench::wipeKeyBuffer(&metadata,sizeof(metadata));return ok;
  }
};
}
