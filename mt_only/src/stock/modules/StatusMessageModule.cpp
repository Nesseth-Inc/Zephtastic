// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/StatusMessageModule.cpp
#include "StatusMessageModule.h"

namespace mt::stock {

int32_t StatusMessageModule::runOnce(const char (&nodeStatus)[80], const StatusMessageSender &service)
{
    if (nodeStatus[0] != '\0' && service.accepts(nodeStatus)) {
        meshtastic_StatusMessage ourStatus = meshtastic_StatusMessage_init_zero;
        strncpy(ourStatus.status, nodeStatus, sizeof(ourStatus.status));
        ourStatus.status[sizeof(ourStatus.status) - 1] = '\0';

        // ZT's background queue supplies the primary-channel broadcast envelope and priority.
        meshtastic_Data p = meshtastic_Data_init_zero;
        if (!service.encode(p, ourStatus))
            return IntervalMs;
        p.portnum = meshtastic_PortNum_NODE_STATUS_APP;
        p.want_response = false;
        p.has_bitfield = true;
        service.sendToMesh(p);
    }
    return IntervalMs;
}

bool StatusMessageModule::handleReceived(const ReceivedStatusPacket &mp, const StatusMessageReceiver &nodeDB)
{
    // Companion dispatches only decoded and admitted NODE_STATUS_APP packets here.
    meshtastic_StatusMessage incomingMessage = meshtastic_StatusMessage_init_zero;
    if (mp.decode(incomingMessage))
        return nodeDB.setNodeStatus(mp.from, incomingMessage);
    return false;
}
}
