meshtastic_MeshPacket *NodeInfoModule::allocReply()
{
    // Only apply suppression when actually replying to someone else's request, not for periodic broadcasts.
    const bool isReplyingToExternalRequest = currentRequest &&
                                             currentRequest->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
                                             currentRequest->decoded.portnum == meshtastic_PortNum_NODEINFO_APP &&
                                             currentRequest->decoded.want_response && !isFromUs(currentRequest);

    if (suppressReplyForCurrentRequest && isReplyingToExternalRequest) {
        LOG_DEBUG("Skip send NodeInfo since we heard the requester <12h ago");
        ignoreRequest = true;
        suppressReplyForCurrentRequest = false;
        return NULL;
    }

    if (!airTime->isTxAllowedChannelUtil(false)) {
        ignoreRequest = true; // Mark it as ignored for MeshModule
        LOG_DEBUG("Skip send NodeInfo > 40%% ch. util");
        return NULL;
    }

    // Use graduated scaling based on active mesh size (30 minute base, scales with congestion coefficient)
    uint32_t timeoutMs = Default::getConfiguredOrDefaultMsScaled(0, 30 * 60, nodeStatus->getNumOnline());
    // A licensed station's call-sign announcement is a regulatory interval, not a preference: ham mode
    // sets node_info_broadcast_secs to 600 s, which a set-config would otherwise clamp to an hour.
    // Never hold such a station past its own interval, whatever the floor and the scaling say.
    if (owner.is_licensed) {
        const uint32_t hamMs =
            Default::getConfiguredOrDefaultMs(config.device.node_info_broadcast_secs, default_node_info_broadcast_secs);
        if (hamMs < timeoutMs)
            timeoutMs = hamMs;
    }
    uint32_t lastNodeInfo = transmitHistory ? transmitHistory->getLastSentToMeshMillis(meshtastic_PortNum_NODEINFO_APP) : 0;
    if (!shorterTimeout && lastNodeInfo && Throttle::isWithinTimespanMs(lastNodeInfo, timeoutMs)) {
        LOG_DEBUG("Skip send NodeInfo since we sent it <%us ago", timeoutMs / 1000);
        ignoreRequest = true; // Mark it as ignored for MeshModule
        return NULL;
    } else if (shorterTimeout && lastNodeInfo && Throttle::isWithinTimespanMs(lastNodeInfo, 60 * 1000)) {
        // For interactive/urgent requests (e.g., user-triggered or implicit requests), use a shorter 60s timeout
        LOG_DEBUG("Skip send NodeInfo since we sent it <60s ago");
        ignoreRequest = true;
        return NULL;
    } else {
        ignoreRequest = false; // Don't ignore requests anymore
        meshtastic_User u = owner;

        // FIXME: Clear the user.id field since it should be derived from node number on the receiving end
        // u.id[0] = '\0';

        // Ensure our user.id is derived correctly
        strcpy(u.id, nodeDB->getNodeId().c_str());

        LOG_INFO("Send owner %s/%s/%s", u.id, u.long_name, u.short_name);
        // The framework sends its own reply, so stamp here for that path. sendOurNodeInfo() stamps
        // after the router accepts the packet instead - a send that never went out must not throttle
        // the next one, and the floor it would sit out is 30 minutes.
        if (transmitHistory && !deferHistoryStamp)
            transmitHistory->setLastSentToMesh(meshtastic_PortNum_NODEINFO_APP);
        return allocDataProtobuf(u);
    }
}
