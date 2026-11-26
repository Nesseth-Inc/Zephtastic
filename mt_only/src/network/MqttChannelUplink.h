// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../NodeUser.h"
#include "MqttDataPolicy.h"
#include "MqttPacketEligibility.h"
#include "../Config.h"
#include <stdio.h>

namespace mt::network {
// Pure RAM bridge. The caller owns the active ChannelScope and the Service
// serialization. Neither this helper nor its sink performs socket operations.
class MqttChannelUplink {
public:
  using Sink=bool(*)(void*,const char*,const meshtastic_MeshPacket*,const char*,const char*);
  static bool topic(const char *root,const char *channel,const char *gateway,char *out,size_t size) {
    if(!root||!channel||!*channel||!gateway||!*gateway||!out)return false;
    const char *parts[]={root,channel,gateway};
    for(const char *s:parts)for(size_t i=0;s[i];++i)
      if(static_cast<uint8_t>(s[i])<0x20||s[i]=='+'||s[i]=='#')return false;
    if(strchr(channel,'/')||strchr(gateway,'/'))return false;
    const int n=snprintf(out,size,"%s/2/e/%s/%s",*root?root:"msh",channel,gateway);
    return n>0&&size_t(n)<size;
  }
  static bool publish(bench::MtCodec &codec,const Config &config,unsigned channelIndex,
                      const uint8_t *frame,size_t length,const bench::RouteMetadata &metadata,
                      const MqttPacketEligibility::Context &context,const char *root,
                      const char *gateway,Sink sink,void *opaque) {
    if(!sink||!frame||length<17||length>255||channelIndex>=8||
       (frame[12]&7)>((frame[12]>>5)&7)||
       !metadata.channelDecoded||metadata.decodeRejected||metadata.backendFailed||
       metadata.signatureRejected||metadata.signatureQuarantined)return false;
    uint8_t data[239]{};size_t dataSize=0;
    const bool canonical=MqttDataPolicy::encode(metadata,data,sizeof(data),dataSize)&&
      codec.matchesChannelData(frame,length,data,dataSize);
    if(!canonical){bench::wipeKeyBuffer(data,sizeof(data));return false;}
    meshtastic_MeshPacket encrypted{},decoded{};
    encrypted.from=decoded.from=bench::le32(frame+4);
    encrypted.to=decoded.to=bench::le32(frame);
    encrypted.id=decoded.id=bench::le32(frame+8);
    encrypted.channel=frame[13];decoded.channel=channelIndex;
    encrypted.hop_limit=decoded.hop_limit=frame[12]&7;
    encrypted.hop_start=decoded.hop_start=(frame[12]>>5)&7;
    encrypted.want_ack=decoded.want_ack=(frame[12]&8)!=0;
    encrypted.via_mqtt=decoded.via_mqtt=(frame[12]&16)!=0;
    encrypted.next_hop=decoded.next_hop=frame[14];
    encrypted.relay_node=decoded.relay_node=frame[15];
    encrypted.which_payload_variant=meshtastic_MeshPacket_encrypted_tag;
    encrypted.encrypted.size=pb_size_t(length-16);
    memcpy(encrypted.encrypted.bytes,frame+16,length-16);
    decoded.which_payload_variant=meshtastic_MeshPacket_decoded_tag;
    auto input=pb_istream_from_buffer(data,dataSize);
    bool ok=pb_decode(&input,meshtastic_Data_fields,&decoded.decoded)&&!input.bytes_left;
    const auto decision=ok?MqttPacketEligibility::evaluate(context,&encrypted,&decoded,data,dataSize,metadata.signatureVerified):
      MqttPacketEligibility::Decision::Deny;
    if(decision==MqttPacketEligibility::Decision::CanonicalDecodedPublic&&!decoded.decoded.xeddsa_signature.size)
      NodeUser::coerceId(decoded.decoded,decoded.from);
    ChannelRecord record;char topicBytes[160]{};
    const bool haveChannel=config.channelRecord(channelIndex,record)&&record.role()!=0;
    const char *name=haveChannel?(record.name()[0]?record.name():config.modemName()):"";
    ok=decision!=MqttPacketEligibility::Decision::Deny&&haveChannel&&
      topic(root,name,gateway,topicBytes,sizeof(topicBytes))&&
      sink(opaque,topicBytes,decision==MqttPacketEligibility::Decision::OriginalEncryptedEnvelope?
        &encrypted:&decoded,name,gateway);
    bench::wipeKeyBuffer(data,sizeof(data));bench::wipeKeyBuffer(&decoded,sizeof(decoded));
    bench::wipeKeyBuffer(&encrypted,sizeof(encrypted));return ok;
  }
};
}
