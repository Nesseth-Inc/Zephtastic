// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/DetectionSensorModule.cpp
#pragma once

#include <stdint.h>
#include <stdio.h>

namespace mt::stock::detection {

enum Verdict { Detected, SendState, Noop };

inline Verdict detection_trigger_logic_level(bool, bool current)
{
    return current ? Detected : Noop;
}

inline Verdict detection_trigger_single_edge(bool prev, bool current)
{
    return (!prev && current) ? Detected : Noop;
}

inline Verdict detection_trigger_either_edge(bool prev, bool current)
{
    if (prev == current)
        return Noop;
    return current ? Detected : SendState;
}

inline bool hasDetectionEvent(uint32_t trigger, bool currentState)
{
    return (trigger & 1) ? currentState : !currentState;
}

// ZT validates the protobuf enum when it accepts the configuration. The stock
// default for an out-of-range value is LOGIC_LOW (first handler).
inline Verdict triggerVerdict(uint32_t trigger, bool wasDetected, bool isDetected)
{
    switch (trigger) {
    case 2: case 3: return detection_trigger_single_edge(wasDetected, isDetected);
    case 4: case 5: return detection_trigger_either_edge(wasDetected, isDetected);
    default: return detection_trigger_logic_level(wasDetected, isDetected);
    }
}

struct Sample { Verdict verdict; bool heartbeat; };

// DetectionSensorModule::runOnce, between GPIO setup and packet allocation.
// Suppressed samples never advance edge history. A due edge outranks the
// heartbeat; a pending edge is ZT's bounded retry of an earlier allocation.
inline Sample runOnce(uint32_t trigger, bool &previous, bool active,
                      bool minimumDue, bool heartbeatDue, bool retryEdge)
{
    if (minimumDue) {
        const auto verdict = triggerVerdict(trigger, previous, active);
        previous = active;
        if (verdict != Noop)
            return {verdict, false};
    }
    return heartbeatDue && !retryEdge ? Sample{SendState, true} : Sample{Noop, false};
}

// Payloads from sendDetectionMessage and sendCurrentStateMessage. The caller
// supplies a validated, terminated sensor name and retains packet ownership.
inline size_t composeMessage(uint8_t *payload, size_t capacity, const char *name,
                             Verdict verdict, bool state, bool sendBell)
{
    if (verdict == Noop || !capacity)
        return 0;
    char *message = reinterpret_cast<char *>(payload);
    const int length = verdict == Detected ? snprintf(message, capacity, "%s detected", name) :
                                            snprintf(message, capacity, "%s state: %i", name, int(state));
    if (length < 0 || size_t(length) >= capacity)
        return 0;
    size_t size = size_t(length);
    if (verdict == Detected && sendBell && size + 1 < 233 && size + 1 < capacity) {
        payload[size++] = 7;
        payload[size] = 0;
    }
    return size;
}

} // namespace mt::stock::detection
