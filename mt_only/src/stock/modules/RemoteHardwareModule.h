// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, src/modules/RemoteHardwareModule.cpp.
#pragma once
#include <meshtastic/remote_hardware.pb.h>
#include <stdint.h>

namespace mt::stock::RemoteHardwareModule {
constexpr uint32_t WatchIntervalMs = 30000;
inline void readReply(uint64_t mask, uint64_t value, meshtastic_HardwareMessage &r)
{
    r = meshtastic_HardwareMessage_init_default;
    r.type = meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY;
    r.gpio_value = value & mask;
    r.gpio_mask = mask;
}

// ZT supplies operations on a fully admitted mask. Writes use one backend
// transaction; replies retain the request's source, ID and channel.
template<class Write, class Read, class Watch, class Reply, class Result>
Result handleReceivedProtobuf(const meshtastic_HardwareMessage &p, Write write,
                             Read digitalReads, Watch watch, Reply setReplyTo,
                             Result backendFailure, Result noReply)
{
    switch (p.type) {
    case meshtastic_HardwareMessage_Type_WRITE_GPIOS:
        return write(p.gpio_mask, p.gpio_value & p.gpio_mask);

    case meshtastic_HardwareMessage_Type_READ_GPIOS: {
        uint64_t res = 0;
        if (!digitalReads(p.gpio_mask, res))
            return backendFailure;

        meshtastic_HardwareMessage r = meshtastic_HardwareMessage_init_default;
        readReply(p.gpio_mask, res, r);
        return setReplyTo(r);
    }
    case meshtastic_HardwareMessage_Type_WATCH_GPIOS:
        return watch(p.gpio_mask);

    case meshtastic_HardwareMessage_Type_READ_GPIOS_REPLY:
    case meshtastic_HardwareMessage_Type_GPIOS_CHANGED:
    default:
        return noReply;
    }
}

// The caller supplies the sampled pins and monotonic time. An explicit first
// sample flag also forces publication when all 64 watched pins read zero.
inline bool runOnce(bool enabled, uint64_t watchGpios, uint32_t now, uint64_t sampled,
                    uint32_t &lastWatchMsec, uint64_t &previousWatch, bool &initial,
                    meshtastic_HardwareMessage &message)
{
    if (enabled && watchGpios && uint32_t(now - lastWatchMsec) >= WatchIntervalMs) {
        const uint64_t curVal = sampled & watchGpios;
        lastWatchMsec = now;
        if (initial || curVal != previousWatch) {
            previousWatch = curVal;
            initial = false;
            message = meshtastic_HardwareMessage_init_default;
            message.type = meshtastic_HardwareMessage_Type_GPIOS_CHANGED;
            message.gpio_value = curVal;
            return true;
        }
    }
    return false;
}
}
