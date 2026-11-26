// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, src/modules/NodeInfoModule.cpp.
#pragma once
#include <stdint.h>
namespace mt::stock::nodeinfo {
constexpr uint32_t RecentSenderMs = 12u * 60u * 60u * 1000u;
inline bool allocReply(bool channelAllowed,bool haveLast,uint32_t now,uint32_t last,
                       uint32_t routineMs,bool shorter,bool licensed,uint32_t licensedIntervalMs)
{
    if (!channelAllowed) return false;
    if (licensed && licensedIntervalMs < routineMs) routineMs = licensedIntervalMs;
    const uint32_t timeout = shorter ? 60000u : routineMs;
    return !haveLast || uint32_t(now-last) >= timeout;
}
inline bool sendOurNodeInfo(bool wantReplies,uint32_t role)
{
    // TRACKER and SENSOR never solicit the entire mesh in an announcement.
    return wantReplies && role != 5 && role != 6;
}
}
