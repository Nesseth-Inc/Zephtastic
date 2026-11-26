// SPDX-License-Identifier: GPL-3.0-only
// WaypointStore::clearWireNotificationPreferences, Meshtastic 8e6a88d.
// Strip the three local-only fields from a phone-originated radio copy in place.
#pragma once
#include <pb_decode.h>
#include "meshtastic/mesh.pb.h"
#include <string.h>
namespace mt::stock::waypoint {
inline size_t stripLocalPreferences(uint8_t *wire,size_t length)
{
    auto input=pb_istream_from_buffer(wire,length);
    size_t kept=0;
    while(input.bytes_left){
        const size_t start=length-input.bytes_left;
        pb_wire_type_t type;uint32_t tag;bool eof;
        const bool tagged=pb_decode_tag(&input,&type,&tag,&eof);
        const bool preference=tagged&&(tag==meshtastic_Waypoint_notify_on_enter_tag||
                                      tag==meshtastic_Waypoint_notify_on_exit_tag||
                                      tag==meshtastic_Waypoint_notify_favorites_only_tag);
        // A malformed opaque suffix has no decodable fields beyond this point.
        // Keep the existing opaque carrier contract unless this is itself a preference.
        const bool complete=tagged&&pb_skip_field(&input,type);
        const size_t end=complete?length-input.bytes_left:length;
        if(!preference){memmove(wire+kept,wire+start,end-start);kept+=end-start;}
        if(!complete)break;
    }
    return kept;
}
}
