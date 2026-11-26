// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../PeerStatus.h"
#include "meshtastic/mesh.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>
#include <string.h>

namespace mt::stock {

// Non-owning callbacks are synchronous. The application owner supplies its current
// Service/NodeCache, and independently handles channel, signature and queue admission.
class StatusMessageSender {
public:
    template <class Send> explicit StatusMessageSender(Send &send)
        : context_(&send), send_([](void *context, const meshtastic_Data &data) {
            (*static_cast<Send *>(context))(data);
        }) {}

    static bool accepts(const char (&nodeStatus)[80]) {
        return memchr(nodeStatus, '\0', sizeof(nodeStatus)) != nullptr;
    }
    static bool encode(meshtastic_Data &data, const meshtastic_StatusMessage &status) {
        auto stream = pb_ostream_from_buffer(data.payload.bytes, sizeof(data.payload.bytes));
        if (!pb_encode(&stream, meshtastic_StatusMessage_fields, &status))
            return false;
        data.payload.size = stream.bytes_written;
        return true;
    }
    void sendToMesh(const meshtastic_Data &data) const { send_(context_, data); }
private:
    void *context_;
    void (*send_)(void *, const meshtastic_Data &);
};

struct ReceivedStatusPacket {
    uint32_t from;
    const uint8_t *payload;
    size_t size;

    bool decode(meshtastic_StatusMessage &status) const {
        if ((!payload && size) || size > meshtastic_StatusMessage_size)
            return false;
        const uint8_t empty = 0;
        auto in = pb_istream_from_buffer(size ? payload : &empty, size);
        if (!pb_decode(&in, meshtastic_StatusMessage_fields, &status) || in.bytes_left ||
            !memchr(status.status, '\0', sizeof(status.status)))
            return false;

        // A duplicate field, embedded NUL, nonminimal varint or trailing bytes cannot replace a cached status.
        uint8_t canonical[meshtastic_StatusMessage_size];
        auto out = pb_ostream_from_buffer(canonical, sizeof(canonical));
        return pb_encode(&out, meshtastic_StatusMessage_fields, &status) && out.bytes_written == size &&
               (!size || !memcmp(canonical, payload, size));
    }
};

class StatusMessageReceiver {
public:
    template <class Store> explicit StatusMessageReceiver(Store &store)
        : context_(&store), store_([](void *context, uint32_t from, const PeerStatus &status) {
            return (*static_cast<Store *>(context))(from, status);
        }) {}

    bool setNodeStatus(uint32_t from, const meshtastic_StatusMessage &incomingMessage) const {
        PeerStatus status;
        return status.assign(incomingMessage.status) && store_(context_, from, status);
    }
private:
    void *context_;
    bool (*store_)(void *, uint32_t, const PeerStatus &);
};
}
