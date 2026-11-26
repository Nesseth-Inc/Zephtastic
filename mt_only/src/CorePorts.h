// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace mt {

// Allowlist of the 13 decoded portnums accepted by the stock
// CORE_PORTNUMS_ONLY predicate (pinned stock 8e6a88d0).
constexpr bool corePort(uint32_t port) {
    switch (port) {
    case 1:   // TEXT_MESSAGE_APP
    case 3:   // POSITION_APP
    case 4:   // NODEINFO_APP
    case 5:   // ROUTING_APP
    case 6:   // ADMIN_APP
    case 7:   // TEXT_MESSAGE_COMPRESSED_APP
    case 8:   // WAYPOINT_APP
    case 11:  // ALERT_APP
    case 12:  // KEY_VERIFICATION_APP
    case 35:  // STORE_FORWARD_PLUSPLUS_APP
    case 65:  // STORE_FORWARD_APP
    case 67:  // TELEMETRY_APP
    case 70:  // TRACEROUTE_APP
        return true;
    default:
        return false;
    }
}

}  // namespace mt
