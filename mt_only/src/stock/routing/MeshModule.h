// SPDX-License-Identifier: GPL-3.0-only
// Reply envelope policy from Meshtastic d5f1619 src/mesh/MeshModule.cpp.
#pragma once

#include "RoutingModule.h"
#include <meshtastic/mesh.pb.h>

namespace mt::stock::routing {
inline void setReplyTo(meshtastic_MeshPacket &reply, const meshtastic_MeshPacket &request,
                       uint32_t localNode, unsigned configuredHopLimit)
{
    reply.to = request.from ? request.from : localNode;
    reply.channel = request.channel;
    reply.hop_limit = getHopLimitForResponse(request.hop_start, request.hop_limit, configuredHopLimit,
        request.which_payload_variant == meshtastic_MeshPacket_decoded_tag && request.decoded.has_bitfield);
    reply.want_ack = request.from != 0 && request.want_ack;
    if (reply.priority == meshtastic_MeshPacket_Priority_UNSET)
        reply.priority = meshtastic_MeshPacket_Priority_RELIABLE;
    reply.decoded.request_id = request.id;
}
}
