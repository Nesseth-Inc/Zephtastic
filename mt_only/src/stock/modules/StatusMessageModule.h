// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/StatusMessageModule.h
#pragma once
#include "StatusMessagePlatform.h"

namespace mt::stock {
class StatusMessageModule {
public:
    static constexpr uint32_t FirstDelayMs = 2 * 60 * 1000;
    static constexpr uint32_t IntervalMs = 12 * 60 * 60 * 1000;
    static int32_t runOnce(const char (&nodeStatus)[80], const StatusMessageSender &service);
    static bool handleReceived(const ReceivedStatusPacket &mp, const StatusMessageReceiver &nodeDB);
};
}
