// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d, TrafficManagementModule::directResponseAllowed/directResponseSlot.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mt::stock {
class DirectResponseThrottle {
    struct Entry { uint32_t key=0; uint64_t lastReplyMs=0; };
    static constexpr size_t kDirectResponseTrackedNodes=8;
    Entry directRequesterSeen[kDirectResponseTrackedNodes]{};
    Entry directTargetSeen[kDirectResponseTrackedNodes]{};
    uint64_t lastDirectResponseMs=0;
    bool seenDirectResponse=false;

    static Entry *directResponseSlot(Entry (&table)[kDirectResponseTrackedNodes],
                                     uint32_t key, uint64_t nowMs, uint64_t windowMs) {
        for (auto &entry : table) {
            if (entry.key == key)
                return nowMs < entry.lastReplyMs || nowMs-entry.lastReplyMs < windowMs ? nullptr : &entry;
        }
        Entry *slot=&table[0];
        for (auto &entry : table) {
            if (!entry.key) return &entry;
            if (entry.lastReplyMs < slot->lastReplyMs) slot=&entry;
        }
        return slot;
    }
public:
    static constexpr uint64_t PerRequesterMs=60000, PerTargetMs=60000, GlobalFloorMs=1000;
    struct Reservation {
        friend class DirectResponseThrottle;
    private:
        Entry *requester=nullptr, *target=nullptr;
    };

    // All callers run on the cooperative service thread. The Zephyr owner uses
    // a 64-bit monotonic clock and stamps only when its bounded send succeeds.
    bool directResponseAllowed(uint32_t requester, uint32_t target, uint64_t nowMs, Reservation &slots) {
        if (seenDirectResponse && (nowMs < lastDirectResponseMs || nowMs-lastDirectResponseMs < GlobalFloorMs))
            return false;
        slots.requester=directResponseSlot(directRequesterSeen,requester,nowMs,PerRequesterMs);
        if (!slots.requester) return false;
        slots.target=directResponseSlot(directTargetSeen,target,nowMs,PerTargetMs);
        return slots.target != nullptr;
    }
    void accepted(const Reservation &slots, uint32_t requester, uint32_t target, uint64_t nowMs) {
        *slots.requester={requester,nowMs};
        *slots.target={target,nowMs};
        lastDirectResponseMs=nowMs;
        seenDirectResponse=true; // timestamp zero is a valid boot instant.
    }
};
}
