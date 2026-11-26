// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, MQTT::onClientProxyReceive in src/mqtt/MQTT.cpp.
#pragma once
#include <meshtastic/mesh.pb.h>
#include <string.h>

namespace mt::stock::MQTT {
template<class Receive>
bool onClientProxyReceive(const meshtastic_MqttClientProxyMessage &msg, Receive onReceive) {
    switch (msg.which_payload_variant) {
    case meshtastic_MqttClientProxyMessage_data_tag:
        if (msg.payload_variant.data.size > sizeof(msg.payload_variant.data.bytes)) return false;
        return onReceive(msg.topic, msg.payload_variant.data.bytes, msg.payload_variant.data.size);
    case meshtastic_MqttClientProxyMessage_text_tag: {
        const char *text = msg.payload_variant.text;
        const size_t capacity = sizeof(msg.payload_variant.text);
        const size_t length = strnlen(text, capacity);
        // Reject unterminated or ambiguous strings at the phone boundary.
        if (length == capacity) return false;
        for (size_t i = length + 1; i < capacity; ++i)
            if (text[i]) return false;
        return onReceive(msg.topic, reinterpret_cast<const uint8_t *>(text), length);
    }
    default:
        return false;
    }
}
}
