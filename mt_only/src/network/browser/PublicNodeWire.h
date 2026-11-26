// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
namespace mt::network::browser::node {
using Labels=meshtastic_User;
using Peer=meshtastic_NodeInfo;
using Envelope=meshtastic_FromRadio;
// Use the stock layouts/tags for public labels. Skipping every other field in
// nanopb keeps future enum values representable on a short-enum receiver.
#define Labels_FIELDLIST(X,a) \
 X(a,STATIC,SINGULAR,STRING,long_name,meshtastic_User_long_name_tag) \
 X(a,STATIC,SINGULAR,STRING,short_name,meshtastic_User_short_name_tag)
#define Labels_DEFAULT NULL
#define Labels_CALLBACK NULL
PB_BIND(Labels,Labels,2)
#define Peer_FIELDLIST(X,a) \
 X(a,STATIC,SINGULAR,UINT32,num,meshtastic_NodeInfo_num_tag) \
 X(a,STATIC,OPTIONAL,MESSAGE,user,meshtastic_NodeInfo_user_tag)
#define Peer_user_MSGTYPE Labels
#define Peer_DEFAULT NULL
#define Peer_CALLBACK NULL
PB_BIND(Peer,Peer,2)
#define Envelope_FIELDLIST(X,a) \
 X(a,STATIC,SINGULAR,UINT32,id,meshtastic_FromRadio_id_tag) \
 X(a,STATIC,ONEOF,MESSAGE,(payload_variant,node_info,node_info),meshtastic_FromRadio_node_info_tag)
#define Envelope_payload_variant_node_info_MSGTYPE Peer
#define Envelope_DEFAULT NULL
#define Envelope_CALLBACK NULL
PB_BIND(Envelope,Envelope,2)
#undef Labels_FIELDLIST
#undef Labels_DEFAULT
#undef Labels_CALLBACK
#undef Peer_FIELDLIST
#undef Peer_user_MSGTYPE
#undef Peer_DEFAULT
#undef Peer_CALLBACK
#undef Envelope_FIELDLIST
#undef Envelope_payload_variant_node_info_MSGTYPE
#undef Envelope_DEFAULT
#undef Envelope_CALLBACK
inline bool isNodeRecord(pb_istream_t stream) {
    pb_wire_type_t type;uint32_t tag;bool eof=false;
    if(!pb_decode_tag(&stream,&type,&tag,&eof))return false;
    // The Companion emits canonical records, with its optional sequence first.
    if(tag==meshtastic_FromRadio_id_tag){
        if(type!=PB_WT_VARINT || !pb_skip_field(&stream,type)||!pb_decode_tag(&stream,&type,&tag,&eof))return false;
    }
    return tag==meshtastic_FromRadio_node_info_tag && type==PB_WT_STRING;
}
}
