// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d src/modules/StoreForwardModule.cpp. ZT's Service owns
// identity/channel admission, bounded PSRAM, the transmit queue and the clock.
#pragma once
#include <meshtastic/config.pb.h>
#include <meshtastic/storeforward.pb.h>

namespace mt::stock {
struct StoreForwardModule {
    static bool serverRole(unsigned role, bool configuredServer)
    {
        return role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
               role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE || configuredServer;
    }

    // The stock router zero-initializes its fixed nanopb payload before decode.
    // An addressed two-byte "SF" and the explicit NUL-terminated form both match.
    static bool legacyHistoryRequest(bool addressedToUs, const uint8_t *payload, size_t size)
    {
        return addressedToUs && payload && size >= 2 && payload[0] == 'S' &&
               payload[1] == 'F' && (size == 2 || payload[2] == 0);
    }

    template<class Record>
    static bool available(const Record &r, uint32_t dest, uint32_t cutoff)
    {
        return r.time && r.time > cutoff && r.from != dest &&
               (r.to == UINT32_MAX || r.to == dest);
    }

    template<class Server>
    static void runOnce(Server &server, uint32_t now, bool txAllowed, typename Server::Action &out)
    {
        if (!server.config_.enabled || !server.config_.isServer || !txAllowed)
            return;
        if (server.active_) {
            if (!Server::due(now, server.dueMillis_)) return;
            out.destination = server.requester_;
            out.requestWindowSeconds = server.requestWindowSeconds_;
            out.activateReplay();
            if (server.selected_ >= server.planned_ ||
                !server.archive_.next(server.requester_, server.cutoff_, out.replay)) {
                server.cancel();
                out.kind = Server::ActionKind::ReplayFinished;
                return;
            }
            out.kind = Server::ActionKind::Replay;
            server.dueMillis_ = now + Server::PacketIntervalMs;
            // The owner finishes after dispatch of the announced count. The donor
            // can otherwise stay busy indefinitely when it hits the return cap.
            out.finalReplay = ++server.selected_ == server.planned_;
        } else if (server.config_.heartbeat &&
                   uint32_t(now - server.lastHeartbeatMillis_) >= server.heartbeatPeriodSeconds_ * 1000u) {
            server.lastHeartbeatMillis_ = now;
            Server::setControl(out, Server::BroadcastNode, meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT);
            out.control.which_variant = meshtastic_StoreAndForward_heartbeat_tag;
            out.control.variant.heartbeat.period = server.heartbeatPeriodSeconds_;
            out.control.variant.heartbeat.secondary = 0;
        }
    }

    template<class Server>
    static void historySend(Server &server, uint32_t secAgo, const typename Server::Context &ctx,
                            typename Server::Action &out)
    {
        const uint32_t last_time = ctx.nowEpochSeconds < secAgo ? 0 : ctx.nowEpochSeconds - secAgo;
        size_t available = 0, lastRequest = 0;
        if (!server.archive_.countAvailable(ctx.peer, last_time, available, &lastRequest))
            return;
        uint32_t queueSize = static_cast<uint32_t>(available);
        if (queueSize > server.config_.returnMax)
            queueSize = server.config_.returnMax;

        if (queueSize) {
            server.active_ = true;
            server.requester_ = ctx.peer;
            server.cutoff_ = last_time;
            server.planned_ = queueSize;
            server.selected_ = 0;
            server.dueMillis_ = ctx.nowMillis + Server::PacketIntervalMs;
        }

        Server::setControl(out, ctx.peer, meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY);
        auto &sf = out.control;
        sf.which_variant = meshtastic_StoreAndForward_history_tag;
        sf.variant.history.history_messages = queueSize;
        sf.variant.history.window = secAgo * 1000;
        sf.variant.history.last_request = static_cast<uint32_t>(lastRequest);
        out.requestWindowSeconds = server.requestWindowSeconds_ = secAgo;
    }

    template<class Server>
    static void statsSend(Server &server, const typename Server::Context &ctx, typename Server::Action &out)
    {
        Server::setControl(out, ctx.peer, meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS);
        auto &sf = out.control;
        sf.which_variant = meshtastic_StoreAndForward_stats_tag;
        sf.variant.stats.messages_total = server.archive_.capacity();
        sf.variant.stats.messages_saved = server.archive_.size();
        sf.variant.stats.messages_max = server.archive_.capacity();
        sf.variant.stats.up_time = ctx.nowMillis / 1000;
        sf.variant.stats.requests = server.requests_;
        sf.variant.stats.requests_history = server.historyRequests_;
        sf.variant.stats.heartbeat = server.config_.heartbeat;
        sf.variant.stats.return_max = server.config_.returnMax;
        sf.variant.stats.return_window = server.config_.returnWindowMinutes;
    }

    template<class Server>
    static void handleReceivedProtobuf(Server &server, const meshtastic_StoreAndForward &p,
                                       const typename Server::Context &ctx, typename Server::Action &out)
    {
        if (!server.config_.enabled || !server.config_.isServer)
            return;
        ++server.requests_;
        switch (p.rr) {
        case meshtastic_StoreAndForward_RequestResponse_CLIENT_ERROR:
        case meshtastic_StoreAndForward_RequestResponse_CLIENT_ABORT:
            if (server.active_ && server.requester_ == ctx.peer) {
                server.cancel();
                out.kind = Server::ActionKind::Cancelled;
            }
            break;
        case meshtastic_StoreAndForward_RequestResponse_CLIENT_HISTORY:
            ++server.historyRequests_;
            if (server.active_ || ctx.defaultChannel) {
                out.kind = Server::ActionKind::ErrorText;
                out.errorText = server.active_ ? Server::ErrorTextKind::Busy : Server::ErrorTextKind::PublicChannel;
                out.countsAsResponse = out.wantResponse = ctx.wantResponse;
            } else {
                const uint32_t minutes = p.which_variant == meshtastic_StoreAndForward_history_tag &&
                                         p.variant.history.window > 0
                                             ? p.variant.history.window : server.config_.returnWindowMinutes;
                historySend(server, minutes * 60, ctx, out);
            }
            break;
        case meshtastic_StoreAndForward_RequestResponse_CLIENT_PING:
            Server::setControl(out, ctx.peer, meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG);
            break;
        case meshtastic_StoreAndForward_RequestResponse_CLIENT_STATS:
            if (server.active_)
                Server::setControl(out, ctx.peer, meshtastic_StoreAndForward_RequestResponse_ROUTER_BUSY);
            else
                statsSend(server, ctx, out);
            break;
        default:
            break;
        }
    }

    // Pinned ROUTER_STATS and ROUTER_HISTORY are informational client state.
    // These reports never change local settings, grants, or archive capacity.
    struct ClientInfo {
        uint32_t records = 0, requests = 0, historyRequests = 0;
        uint32_t returnMax = 25, returnWindowMinutes = 240;
        bool heartbeat = false;
    };

    static void observeRouterInfo(const meshtastic_StoreAndForward &p, ClientInfo &info)
    {
        if (p.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_STATS &&
            p.which_variant == meshtastic_StoreAndForward_stats_tag) {
            const auto &stats = p.variant.stats;
            info.records = stats.messages_max;
            info.requests = stats.requests;
            info.historyRequests = stats.requests_history;
            info.heartbeat = stats.heartbeat;
            info.returnMax = stats.return_max;
            info.returnWindowMinutes = stats.return_window;
        } else if (p.rr == meshtastic_StoreAndForward_RequestResponse_ROUTER_HISTORY &&
                   p.which_variant == meshtastic_StoreAndForward_history_tag) {
            // The client's request uses minutes; the router's response uses milliseconds.
            info.returnWindowMinutes = p.variant.history.window / 60000;
        }
    }

    static bool observeRouterHeartbeat(const meshtastic_StoreAndForward &p, uint32_t now,
                                       uint32_t &lastHeartbeat, uint32_t &heartbeatInterval)
    {
        switch (p.rr) {
        case meshtastic_StoreAndForward_RequestResponse_ROUTER_PONG:
        case meshtastic_StoreAndForward_RequestResponse_ROUTER_HEARTBEAT:
            if (p.which_variant == meshtastic_StoreAndForward_heartbeat_tag)
                heartbeatInterval = p.variant.heartbeat.period;
            lastHeartbeat = now;
            return true;
        default:
            return false;
        }
    }
};
}
