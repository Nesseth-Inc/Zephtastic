// ChannelTrace.h
// SPDX-License-Identifier: GPL-3.0-only
// Pure diagnostic channel trace counters. No I/O, clock, heap, locks,
// floating point, strings, exceptions, or hardware. Never determines
// measurement quality/availability or triggers actions.
#pragma once

#include <cstdint>

namespace zt
{

enum class ChannelEvent : uint8_t
{
    None = 0,
    RxKnown = 1,
    RxUnknown = 2,
    NativeLoss = 3,
    DrainLimit = 4,
    Support = 5,
    TxUncertain = 6,
    PendingOverlap = 7,
    Other = 8
};

struct ChannelTrace
{
    uint32_t rxKnown = 0;
    uint32_t rxUnknown = 0;
    uint32_t uncertain = 0;
    uint32_t loss = 0;
    uint32_t limit = 0;
    uint32_t support = 0;
    uint32_t tx = 0;
    uint32_t overlap = 0;
    uint32_t other = 0;
    uint32_t lastMs = 0;
    uint16_t irq = 0;
    uint8_t len = 0;
    ChannelEvent reason = ChannelEvent::None;
    bool known = false;
};

// Saturating counter increment: stops at UINT32_MAX (no unsigned overflow).
inline void noteChannelTrace(ChannelTrace &s, ChannelEvent event, uint32_t ms = 0, uint16_t irq = 0, uint8_t len = 0)
{
    auto inc = [](uint32_t &v) { if (v != UINT32_MAX) { ++v; } };
    switch (event)
    {
        case ChannelEvent::RxKnown:
            inc(s.rxKnown);
            s.lastMs = ms;
            s.irq = irq;
            s.len = len;
            s.known = true;
            break;
        case ChannelEvent::RxUnknown:
            inc(s.rxUnknown);
            inc(s.uncertain);
            s.reason = ChannelEvent::RxUnknown;
            s.lastMs = ms;
            s.irq = irq;
            s.len = len;
            s.known = false;
            break;
        case ChannelEvent::NativeLoss:
            inc(s.loss);
            inc(s.uncertain);
            s.reason = ChannelEvent::NativeLoss;
            break;
        case ChannelEvent::DrainLimit:
            inc(s.limit);
            inc(s.uncertain);
            s.reason = ChannelEvent::DrainLimit;
            break;
        case ChannelEvent::Support:
            inc(s.support);
            inc(s.uncertain);
            s.reason = ChannelEvent::Support;
            break;
        case ChannelEvent::TxUncertain:
            inc(s.tx);
            inc(s.uncertain);
            s.reason = ChannelEvent::TxUncertain;
            break;
        case ChannelEvent::PendingOverlap:
            inc(s.overlap);
            inc(s.uncertain);
            s.reason = ChannelEvent::PendingOverlap;
            break;
        case ChannelEvent::Other:
            inc(s.other);
            inc(s.uncertain);
            s.reason = ChannelEvent::Other;
            break;
        default:
            break; // None or invalid value: no mutation.
    }
}
} // namespace zt
