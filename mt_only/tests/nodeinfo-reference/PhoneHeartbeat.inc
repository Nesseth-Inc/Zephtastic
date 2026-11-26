        case meshtastic_ToRadio_heartbeat_tag:
            // nonce==1 is a special "nodeinfo ping" trigger: force a fresh
            // NodeInfo broadcast on the 60-second shorterTimeout path so
            // peers can re-learn our public key after a reboot or
            // factory_reset without waiting out the normal 10-minute
            // NodeInfo send cooldown. Mirrors the TCP/UDP path in
            // `src/mesh/api/PacketAPI.cpp:74-79` for serial clients.
            // Default nonce (0) remains a plain keepalive that triggers
            // a queue-status reply.
            if (toRadioScratch.heartbeat.nonce == 1) {
                if (nodeInfoModule) {
                    LOG_INFO("Broadcast nodeinfo ping (serial)");
                    nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, true, 0, true);
                }
            } else {
                LOG_TRACE("Got client heartbeat");
                heartbeatReceived = true;
            }
            break;
