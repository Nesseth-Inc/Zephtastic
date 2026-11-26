// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SettingsJournal.h"
#include "mqtt/OnlineNodeWindow.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

namespace mt {
namespace mqtt {

// Alternating, public-metadata-only snapshots for the bounded hot-node mirror.
// The uptime sidecar is deliberately volatile and is never serialized here.
class HotNodeMetadataJournal {
public:
    static constexpr size_t HeaderSize = 48;
    static constexpr size_t RowSize = 9;
    static constexpr size_t RecordSize = HeaderSize + OnlineNodeWindow::HotCapacity * RowSize + 4;
    static constexpr uint8_t FormatVersion = 1;

    enum class State : uint8_t { Unloaded, Empty, Loaded, Recovered, Invalid, IoError };

private:
    State state_ = State::Unloaded;
    uint32_t generation_ = 0;
    int8_t active_ = -1;

    static const char *key(unsigned slot) { return slot ? "mt_hot/meta1" : "mt_hot/meta0"; }
    static uint32_t get32(const uint8_t *p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    static void put32(uint8_t *p, uint32_t n) {
        for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
    }
    static uint32_t crc32(const uint8_t *p, size_t n) {
        uint32_t c = UINT32_MAX;
        for (size_t i = 0; i < n; ++i) {
            c ^= p[i];
            for (unsigned j = 0; j < 8; ++j)
                c = (c >> 1) ^ (0xedb88320u & uint32_t(-int32_t(c & 1)));
        }
        return ~c;
    }
    static void encode(const OnlineNodeWindow &window, uint32_t self,
                       const uint8_t publicKey[32], uint32_t generation,
                       uint8_t out[RecordSize]) {
        memset(out, 0, RecordSize);
        out[0] = 'M'; out[1] = 'T'; out[2] = 'H'; out[3] = 'J';
        out[4] = FormatVersion;
        out[5] = 0;
        const size_t count = window.hotCount();
        out[6] = uint8_t(count); out[7] = uint8_t(count >> 8);
        put32(out + 8, generation);
        put32(out + 12, self);
        memcpy(out + 16, publicKey, 32);
        for (size_t i = 0; i < count; ++i) {
            const OnlineNodeWindow::HotRow *row = window.hotAt(i);
            uint8_t *encoded = out + HeaderSize + i * RowSize;
            put32(encoded, row->node);
            put32(encoded + 4, row->lastHeard);
            encoded[8] = uint8_t((row->viaMqtt ? 1u : 0u) | (row->evictionFlags << 1));
        }
        put32(out + RecordSize - 4, crc32(out, RecordSize - 4));
    }
    static bool decode(const uint8_t in[RecordSize], int length, uint32_t self,
                       const uint8_t publicKey[32], OnlineNodeWindow::HotRow rows[OnlineNodeWindow::HotCapacity],
                       size_t &count, uint32_t &generation) {
        if (length != int(RecordSize) || in[0] != 'M' || in[1] != 'T' || in[2] != 'H' || in[3] != 'J' ||
            in[4] != FormatVersion || in[5] != 0 ||
            get32(in + RecordSize - 4) != crc32(in, RecordSize - 4)) return false;
        count = size_t(in[6]) | (size_t(in[7]) << 8);
        if (!count || count > OnlineNodeWindow::HotCapacity || !self || get32(in + 12) != self ||
            memcmp(in + 16, publicKey, 32) != 0) return false;
        generation = get32(in + 8);
        if (!generation) return false;
        for (size_t i = 0; i < OnlineNodeWindow::HotCapacity; ++i) {
            const uint8_t *encoded = in + HeaderSize + i * RowSize;
            if (i >= count) {
                for (size_t j = 0; j < RowSize; ++j) if (encoded[j]) return false;
                continue;
            }
            const uint8_t flags = encoded[8];
            const uint32_t node = get32(encoded);
            const uint8_t evictionFlags = uint8_t(flags >> 1);
            // Stock node rows are actual nodes, with self at index zero; node
            // zero is only the empty sentinel in the separate uptime sidecar.
            if (!node || (i == 0 ? node != self : node == self) || (flags & 0xc0u) ||
                (evictionFlags & ~uint8_t(OnlineNodeWindow::KnownEvictionFlags))) return false;
            for (size_t prior = 0; prior < i; ++prior)
                if (rows[prior].node == node) return false;
            rows[i] = {node, get32(encoded + 4), bool(flags & 1u), evictionFlags};
        }
        return true;
    }

public:
    State state() const { return state_; }
    bool ready() const { return state_ == State::Empty || state_ == State::Loaded; }
    uint32_t generation() const { return generation_; }
    int activeSlot() const { return active_; }

    // A valid older slot remains available after a torn newer slot, but the
    // journal becomes read-only (Recovered) until an explicit cold reload or
    // external recovery. This prevents automatic repair/overwrite.
    int load(SettingsBackend &backend, OnlineNodeWindow &window, uint32_t self,
             const uint8_t publicKey[32]) {
        state_ = State::Unloaded; generation_ = 0; active_ = -1;
        if (!self || !publicKey) { state_ = State::Invalid; return -EINVAL; }
        uint8_t raw[RecordSize];
        OnlineNodeWindow::HotRow candidates[2][OnlineNodeWindow::HotCapacity]{};
        size_t counts[2]{}; uint32_t generations[2]{}; bool good[2]{}; bool present[2]{};
        for (unsigned slot = 0; slot < 2; ++slot) {
            const int n = backend.read(key(slot), raw, sizeof(raw));
            if (n == -ENOENT) continue;
            present[slot] = true;
            if (n < 0 && n != -E2BIG) { state_ = State::IoError; return n; }
            good[slot] = decode(raw, n, self, publicKey, candidates[slot], counts[slot], generations[slot]);
        }
        if (!present[0] && !present[1]) {
            // Empty is a non-mutating observation. The caller may seed an
            // explicit first baseline from its authoritative current view.
            state_ = State::Empty;
            return 0;
        }
        if (!good[0] && !good[1]) { state_ = State::Invalid; return -EINVAL; }
        unsigned selected = good[1] ? 1u : 0u;
        bool recovered = (present[0] && !good[0]) || (present[1] && !good[1]);
        if (good[0] && good[1]) {
            const uint32_t delta = generations[1] - generations[0];
            if (!delta || delta == 0x80000000u) { state_ = State::Invalid; return -EINVAL; }
            selected = delta < 0x80000000u ? 1u : 0u;
        }
        if (!window.load(candidates[selected], counts[selected])) { state_ = State::Invalid; return -EINVAL; }
        window.setSelf(self);
        generation_ = generations[selected]; active_ = int8_t(selected);
        state_ = recovered ? State::Recovered : State::Loaded;
        return 0;
    }

    // Persists hot rows only. Volatile uptime sidecar state is intentionally
    // outside this journal. Write outcome uncertainty is terminal in-session.
    int save(SettingsBackend &backend, const OnlineNodeWindow &window, uint32_t self,
             const uint8_t publicKey[32]) {
        if (!ready()) return -EACCES;
        if (!self || !publicKey || window.self() != self || !window.hotCount() ||
            window.hotCount() > OnlineNodeWindow::HotCapacity) return -EINVAL;
        if (active_ >= 0 && generation_ == UINT32_MAX) return -EOVERFLOW;
        if (!validWindow(window, self)) return -EINVAL;
        const unsigned slot = active_ == 0 ? 1u : 0u;
        const uint32_t next = generation_ + 1;
        uint8_t record[RecordSize], readback[RecordSize];
        encode(window, self, publicKey, next, record);
        int result = backend.write(key(slot), record, sizeof(record));
        if (result) { state_ = State::IoError; return result; }
        const int n = backend.read(key(slot), readback, sizeof(readback));
        if (n != int(sizeof(record)) || memcmp(record, readback, sizeof(record))) {
            state_ = State::IoError; return n < 0 ? n : -EIO;
        }
        generation_ = next; active_ = int8_t(slot); state_ = State::Loaded;
        return 0;
    }

private:
    static bool validWindow(const OnlineNodeWindow &window, uint32_t self) {
        const size_t count = window.hotCount();
        if (window.hotAt(0) == nullptr || window.hotAt(0)->node != self) return false;
        for (size_t i = 0; i < count; ++i) {
            const auto *row = window.hotAt(i);
            if (!row || !row->node || (i == 0 ? row->node != self : row->node == self) ||
                (row->evictionFlags & ~uint8_t(OnlineNodeWindow::KnownEvictionFlags))) return false;
            for (size_t j = 0; j < i; ++j)
                if (window.hotAt(j)->node == row->node) return false;
        }
        return true;
    }
};

} // namespace mqtt
} // namespace mt
