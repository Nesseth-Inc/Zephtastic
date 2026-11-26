// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "OptionalModules.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include "meshtastic/mesh.pb.h"

namespace mt::stock::optional {
inline bool exampleRequest(const meshtastic_Data &d){
    return d.portnum==meshtastic_PortNum_REPLY_APP&&d.want_response&&d.has_bitfield&&
           d.bitfield<=3&&!d.request_id&&!d.reply_id&&!d.emoji&&!d.source&&!d.dest;
}
inline bool moduleReply(const meshtastic_Data &d){
    if(d.want_response||!d.has_bitfield||d.bitfield||d.source||d.dest||d.reply_id||d.emoji||d.xeddsa_signature.size)
        return false;
#if defined(CONFIG_MT_REPLY_EXAMPLE)
    if(d.portnum==meshtastic_PortNum_REPLY_APP)
        return d.request_id&&d.payload.size==sizeof(ExampleAnswer)-1&&
               !memcmp(d.payload.bytes,ExampleAnswer,sizeof(ExampleAnswer)-1);
#endif
#if defined(CONFIG_MT_REPLY_BOT)
    static constexpr char prefix[]="🎙️ Mic Check : ";
    if(d.portnum==meshtastic_PortNum_TEXT_MESSAGE_APP)
        return !d.request_id&&d.payload.size>=sizeof(prefix)-1&&d.payload.size<96&&
               !memcmp(d.payload.bytes,prefix,sizeof(prefix)-1);
#endif
    return false;
}
inline bool exactWire(const uint8_t *wire,size_t length,bool request){
    if(!wire||!length||length>239)return false;
    meshtastic_Data d{};
    auto in=pb_istream_from_buffer(wire,length);
    if(!pb_decode(&in,meshtastic_Data_fields,&d)||in.bytes_left||d.xeddsa_signature.size||
       !(request?exampleRequest(d):moduleReply(d)))return false;
    uint8_t check[239];auto out=pb_ostream_from_buffer(check,sizeof(check));
    return pb_encode(&out,meshtastic_Data_fields,&d)&&out.bytes_written==length&&!memcmp(check,wire,length);
}
}
