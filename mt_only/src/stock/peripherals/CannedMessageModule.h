// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c, src/modules/CannedMessageModule.cpp
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::stock::canned {

// splitConfiguredMessages reserves two of its 50 slots for destination and exit.
// Return views into ZT's immutable journal record instead of splitting its bytes
// in place or retaining candidate pointers after a configuration edit.
constexpr unsigned MaxMessages = 48;

template <typename Visit>
inline unsigned splitConfiguredMessages(const char *buffer, size_t size, Visit visit)
{
    if (!size)
        return 0; // ZT also supplies an empty view while content is unavailable.

    size_t start = 0;
    unsigned count = 0;
    for (size_t i = 0; i + 1 < size; ++i) {
        if (buffer[i] == '|') {
            visit(count++, buffer + start, i - start);
            if (count == MaxMessages)
                return count;
            start = i + 1;
        }
    }
    visit(count++, buffer + start, size - start);
    return count;
}

// CannedMessageModule::getNextIndex / getPrevIndex. ZT presents its Back
// item in the same bounded selector.
inline unsigned moveSelection(unsigned current, unsigned count, bool next)
{
    if (!count) return 0;
    if (next) return current >= count - 1 ? 0 : current + 1;
    return current == 0 || current >= count ? count - 1 : current - 1;
}

enum class AssignResult : uint8_t { Changed, Unchanged, IgnoredEmpty, Invalid };

// handleSetCannedMessageModuleMessages ignores an empty value and saves only
// changed text. The caller commits the candidate and its enable flag together.
template <size_t Capacity>
inline AssignResult handleSetCannedMessageModuleMessages(char (&messages)[Capacity],
                                                        const char *value, size_t size)
{
    if (!size)
        return AssignResult::IgnoredEmpty;
    if (!value || size >= Capacity || memchr(value, 0, size))
        return AssignResult::Invalid;
    char next[Capacity]{};
    memcpy(next, value, size);
    if (memcmp(messages, next, Capacity) == 0)
        return AssignResult::Unchanged;
    memcpy(messages, next, Capacity);
    return AssignResult::Changed;
}

// sendText payload preparation. Packet allocation, selected destination, ACK
// tracking, PKI and the final send belong to the existing Service owner.
inline size_t copyTextPayload(uint8_t *payload, size_t capacity, const char *message,
                              size_t size, bool sendBell)
{
    if (!message || !size || size > capacity || size > 233)
        return 0;
    memcpy(payload, message, size);
    if (sendBell && size + 1 < 233 && size < capacity)
        payload[size++] = 7;
    return size;
}

// Adapt the stock picker, ACK and channel fields to the two ZT submission
// owners. The private owner verifies the peer and reserves its own packet ID.
template<class Broadcast, class Direct>
int sendText(uint32_t destination, unsigned channel, const uint8_t *payload, size_t size,
             Broadcast broadcast, Direct direct)
{
    constexpr bool wantAck = true;
    if (destination == UINT32_MAX)
        return broadcast(payload, size, channel, wantAck);
    return direct(destination, payload, size, channel, wantAck);
}

} // namespace mt::stock::canned
