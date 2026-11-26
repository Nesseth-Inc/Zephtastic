// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/SerialModule.cpp
#pragma once

#include "../../serial/SerialEngine.h"
#include <string.h>

namespace mt::stock::serial {

// SerialModuleRadio::handleReceived. The adapter supplies an admitted packet,
// the resolved short name and the UART writer. Position ports use meshWaypoint.
inline void handleReceived(mt_serial::Mode mode, bool echo, uint32_t &lastRxID,
                           bool fromUs, uint32_t id, const char *sender,
                           const uint8_t *bytes, size_t size, mt_serial::Write write, void *context)
{
    using mt_serial::Mode;
    if (mode == Mode::Proto)
        return;

    // The upstream printf("%.*s") stops on embedded NUL. write(bytes, size)
    // preserves embedded NULs on the foreign SIMPLE/DEFAULT binary path.
    const auto printPayload = [&]() {
        const auto *end = size ? static_cast<const uint8_t *>(memchr(bytes, 0, size)) : nullptr;
        write(context, bytes, end ? size_t(end - bytes) : size);
    };

    if (fromUs) {
        if (echo && lastRxID != id) {
            lastRxID = id;
            printPayload();
        }
    } else if (mode == Mode::Simple) {
        write(context, bytes, size);
    } else if (mode == Mode::TextMessage) {
        static constexpr uint8_t newline[] = {'\r', '\n'};
        static constexpr uint8_t separator[] = {':', ' '};
        const char *name = sender ? sender : "???";
        write(context, newline, sizeof(newline));
        write(context, reinterpret_cast<const uint8_t *>(name), strlen(name));
        write(context, separator, sizeof(separator));
        printPayload();
        write(context, newline, sizeof(newline));
    }
}

} // namespace mt::stock::serial
