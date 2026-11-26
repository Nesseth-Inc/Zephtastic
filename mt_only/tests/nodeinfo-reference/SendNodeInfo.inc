bool NodeInfoModule::sendOurNodeInfo(NodeNum dest, bool wantReplies, uint8_t channel, bool _shorterTimeout)
{
    // cancel any not yet sent (now stale) position packets
    if (prevPacketId) // if we wrap around to zero, we'll simply fail to cancel in that rare case (no big deal)
        service->cancelSending(prevPacketId);
    shorterTimeout = _shorterTimeout;
    deferHistoryStamp = true;
    DEBUG_HEAP_BEFORE;
    meshtastic_MeshPacket *p = allocReply();
    DEBUG_HEAP_AFTER("NodeInfoModule::sendOurNodeInfo", p);
    deferHistoryStamp = false;

    if (p) { // Check whether we didn't ignore it
        p->to = dest;
        bool requestWantResponse = (config.device.role != meshtastic_Config_DeviceConfig_Role_TRACKER &&
                                    config.device.role != meshtastic_Config_DeviceConfig_Role_SENSOR) &&
                                   wantReplies;

        p->decoded.want_response = requestWantResponse;
        if (_shorterTimeout)
            p->priority = meshtastic_MeshPacket_Priority_DEFAULT;
        else
            p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
        if (channel > 0) {
            LOG_DEBUG("Send ourNodeInfo to channel %d", channel);
            p->channel = channel;
        }

        prevPacketId = p->id;

        const ErrorCode res = service->sendToMesh(p);
        shorterTimeout = false;
        // A rejected send never reached the air, so it neither defers the routine broadcast nor
        // consumes a pending channel change. sendToMesh() has already released the packet.
        if (res != ERRNO_OK && res != ERRNO_SHOULD_RELEASE) {
            LOG_WARN("NodeInfo send rejected (err=%d)", res);
            return false;
        }
        if (transmitHistory)
            transmitHistory->setLastSentToMesh(meshtastic_PortNum_NODEINFO_APP);
        // Our NodeInfo just went on the air, so the routine broadcast is due a full interval from now
        // rather than from the last tick - an ad-hoc send otherwise leaves the periodic copy right behind it.
        setIntervalFromNow(
            Default::getConfiguredOrDefaultMs(config.device.node_info_broadcast_secs, default_node_info_broadcast_secs));
        return true;
    }
    return false;
}
