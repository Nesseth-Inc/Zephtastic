// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, src/modules/TraceRouteModule.cpp. ZT supplies validated
// header distance and the driver's integer dB reading instead of MeshPacket.
#pragma once
#include "meshtastic/mesh.pb.h"

namespace mt::stock {
struct TraceRouteModule {
    struct Path {
        uint32_t *nodes;
        pb_size_t &count;
        int8_t *signals;
        pb_size_t &signalCount;
    };
    static Path path(meshtastic_RouteDiscovery &r, bool towards) {
        if (towards) return {r.route, r.route_count, r.snr_towards, r.snr_towards_count};
        return {r.route_back, r.route_back_count, r.snr_back, r.snr_back_count};
    }
    static void insertUnknownHops(Path p, unsigned distance) {
        while (p.count < distance && p.count < 8) p.nodes[p.count++] = UINT32_MAX;
        while (p.signalCount < p.count && p.signalCount < 8) p.signals[p.signalCount++] = INT8_MIN;
    }
    static void appendMyIDandSNR(Path p, int8_t snr, uint32_t self, bool signalOnly) {
        const int q4 = 4 * int(snr);
        if (p.signalCount < 8)
            p.signals[p.signalCount++] = int8_t(q4 < -127 ? -127 : q4 > 127 ? 127 : q4);
        if (!signalOnly && p.count < 8) p.nodes[p.count++] = self;
    }
};
}
