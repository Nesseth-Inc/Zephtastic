// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic d5f16191ca28b01b5024f590f04a67f25d66f1b1, src/modules/NeighborInfoModule.cpp
#pragma once
#include "../../NeighborObservations.h"
#include "../../NeighborIntervals.h"
#include "../../NeighborInfoPayload.h"
#include <pb_encode.h>

namespace mt::stock {

struct NeighborInfoModule {
    // NeighborObservations owns ZT's fixed table, ignored-peer filter and monotonic clock.
    static void handleReceivedProtobuf(NeighborObservations &neighbors, uint32_t from, uint32_t self,
                                       unsigned hops, uint64_t now, bool snrKnown, float snr,
                                       uint32_t localInterval, const meshtastic_NeighborInfo *np)
    {
        if (np) {
            // Ignore the single-entry interceptable dummy packet.
            if (from && (np->neighbors_count != 1 || np->neighbors[0].node_id != 0 || np->neighbors[0].snr != 0.0f))
                neighbors.observeAnnouncement(from, self, np->last_sent_by_id, np->node_broadcast_interval_secs,
                                              localInterval, hops, now, snrKnown, snr);
        } else if (hops == 0) {
            neighbors.observe(from, self, hops, now, snrKnown, snr, localInterval);
        }
    }

    static uint8_t relayByte(uint32_t node) { return uint8_t(node) ? uint8_t(node) : 0xff; }

    // Resolve only fresh direct sightings. A matching low byte alone is ambiguous.
    static uint32_t lastTransmitter(const NodeCache &nodes, uint32_t self, uint32_t from,
                                    int hops, uint8_t relay, uint32_t utc, const meshtastic_NeighborInfo &info)
    {
        if (hops == 0) return from;
        if (hops < 0) return info.last_sent_by_id;
        if (info.last_sent_by_id && info.last_sent_by_id != info.node_id &&
            relayByte(info.last_sent_by_id) == relay) return info.last_sent_by_id;
        if (!relay || !utc) return 0;
        uint32_t match = 0;
        for (unsigned i = 0; i < nodes.count(); ++i) {
            const auto *entry = nodes.get(i);
            if (entry->id() == self || entry->ignored() || relayByte(entry->id()) != relay ||
                !entry->heard || entry->hops || !entry->lastHeard() ||
                int32_t(utc - entry->lastHeard()) >= 7200) continue;
            if (match) return 0;
            match = entry->id();
        }
        return match;
    }

    template<class ChannelAllowed, class AirAllowed, class Send>
    static uint32_t runOnce(uint32_t configuredInterval, bool transmitOverLoRa, bool allowedPrimary,
                            ChannelAllowed channelAllowed, AirAllowed airAllowed, Send sendNeighborInfo)
    {
        // ZT permits only custom primary keys. An unavailable utilization
        // measurement leaves the packet on the companion-only path.
        const bool overLoRa=transmitOverLoRa && allowedPrimary && channelAllowed() && airAllowed();
        sendNeighborInfo(overLoRa);
        return NeighborIntervals::steadyMs(configuredInterval);
    }

    template <class Send>
    static void sendNeighborInfo(NeighborObservations &neighbors, const NodeCache &cache,
                                 uint32_t self, uint32_t interval, uint64_t now, Send sendToMesh)
    {
        meshtastic_NeighborInfo neighborInfo = meshtastic_NeighborInfo_init_zero;
        neighbors.snapshot(cache, self, interval, now, neighborInfo);
        if (neighborInfo.neighbors_count > 0) {
            uint8_t payload[233];
            auto stream = pb_ostream_from_buffer(payload, sizeof(payload));
            if (pb_encode(&stream, meshtastic_NeighborInfo_fields, &neighborInfo))
                sendToMesh(payload, stream.bytes_written);
        }
    }
};
}
