// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, NeighborInfoModule::getOrCreateNeighbor,
// cleanUpNeighbors and collectNeighborInfo from src/modules/NeighborInfoModule.cpp.
#pragma once
#include <meshtastic/mesh.pb.h>
#include <stdint.h>
#include <stddef.h>

namespace mt::stock {
class NeighborTable {
    struct Entry {
        uint64_t observedMs=0;
        uint32_t node_id=0, node_broadcast_interval_secs=0;
        float snr=0;
        bool direct=false;
    };
    static constexpr size_t Capacity=sizeof(meshtastic_NeighborInfo::neighbors)/sizeof(meshtastic_Neighbor);
    Entry neighbors[Capacity]{};
    size_t size() const {
        size_t n=0;
        while (n<Capacity && neighbors[n].node_id) ++n;
        return n;
    }
    void erase(size_t index, size_t count) {
        for (size_t i=index+1; i<count; ++i) neighbors[i-1]=neighbors[i];
        neighbors[count-1]={};
    }
public:
    void clear() { for (auto &entry : neighbors) entry={}; }
    bool direct(uint32_t id) const {
        for (const auto &entry : neighbors)
            if (entry.node_id==id) return entry.direct;
        return false;
    }

    // The production caller admits IDs and bounded intervals, drops self, and
    // provides SNR, directness and local monotonic time from the RX metadata.
    void getOrCreateNeighbor(uint32_t originalSender, uint32_t id, uint32_t advertisedInterval,
                             uint32_t localInterval, float snr, uint64_t now, bool direct) {
        const bool useAdvertised=originalSender==id && advertisedInterval!=0;
        size_t count=size();
        for (size_t i=0; i<count; ++i) {
            auto &entry=neighbors[i];
            if (entry.node_id!=id) continue;
            entry.snr=snr;
            entry.observedMs=now;
            entry.direct=direct;
            if (useAdvertised) entry.node_broadcast_interval_secs=advertisedInterval;
            return;
        }
        if (count==Capacity) {
            erase(0,count);
            --count;
        }
        neighbors[count]={now,id,useAdvertised ? advertisedInterval : localInterval,snr,direct};
    }

    template<class Ignored>
    void cleanUpNeighbors(uint64_t now, Ignored ignored) {
        size_t count=size();
        for (size_t i=count; i>0; --i) {
            const auto &entry=neighbors[i-1];
            // Source uses epoch seconds. ZT refuses rollback and compares
            // local elapsed seconds to twice the advertised interval in 64 bits.
            if (now<entry.observedMs || now/1000-entry.observedMs/1000>
                uint64_t(entry.node_broadcast_interval_secs)*2 || ignored(entry.node_id))
                erase(i-1,count--);
        }
    }

    template<class Ignored>
    void collectNeighborInfo(meshtastic_NeighborInfo &out, uint32_t self, uint32_t interval,
                             uint64_t now, Ignored ignored) {
        out.node_id=self;
        out.last_sent_by_id=0;
        out.node_broadcast_interval_secs=interval;
        cleanUpNeighbors(now,ignored);
        for (const auto &neighbor : neighbors) {
            if (!neighbor.node_id) break;
            if (neighbor.node_id==self) continue;
            auto &entry=out.neighbors[out.neighbors_count++];
            entry.node_id=neighbor.node_id;
            entry.snr=neighbor.snr;
            // Time and advertised interval are deliberately absent on the radio.
        }
    }
};
}
