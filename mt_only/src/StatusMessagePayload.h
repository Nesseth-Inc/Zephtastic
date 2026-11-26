// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "stock/modules/StatusMessageModule.h"

namespace mt {
struct StatusMessagePayload {
    enum class Result : uint8_t { Disabled, Encoded, Invalid };

    static bool decode(const uint8_t *data, size_t size, PeerStatus &value) {
        auto store = [&](uint32_t, const PeerStatus &status) { value = status; return true; };
        return stock::StatusMessageModule::handleReceived({0, data, size}, stock::StatusMessageReceiver(store));
    }

    static Result encode(const char (&status)[80], uint8_t *out, size_t capacity, size_t &size) {
        size = 0;
        if (!status[0]) return Result::Disabled;
        auto send = [&](const meshtastic_Data &data) {
            if (!out || data.payload.size > capacity) return;
            memcpy(out, data.payload.bytes, data.payload.size);
            size = data.payload.size;
        };
        stock::StatusMessageModule::runOnce(status, stock::StatusMessageSender(send));
        return size ? Result::Encoded : Result::Invalid;
    }
};
}
