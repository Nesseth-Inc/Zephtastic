// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

namespace mt {
namespace mqtt {

// Volatile mirror of the subset used by pinned stock NodeDB's online-local
// count and unknown-ID hot-row admission. It does not own application
// membership or persistence: callers mirror load/remove/evict events, supply
// the exact NodeDB isFull() predicate, and publish policy flags from current
// NodeInfo. The explicit create()/observe() hooks remain available when an
// owner already performed selection itself.
class OnlineNodeWindow {
public:
    static constexpr size_t HotCapacity = 120;
    static constexpr size_t HeardAtCapacity = 32;
    static constexpr uint32_t OnlineSeconds = 60u * 60u * 2u;
    static constexpr uint32_t FullAdmissionIntervalMs = 2000u;

    enum EvictionFlag : uint8_t {
        Favorite = 1u << 0,
        Ignored = 1u << 1,
        ManuallyVerified = 1u << 2,
        HasPublicKey = 1u << 3,
        KnownEvictionFlags = Favorite | Ignored | ManuallyVerified | HasPublicKey,
    };

    struct HotRow {
        uint32_t node;
        uint32_t lastHeard;
        bool viaMqtt;
        uint8_t evictionFlags = 0;
    };

    struct HeardAt {
        uint32_t node;
        uint32_t uptimeSeconds;
    };

    OnlineNodeWindow() { clear(); }

    void clear() {
        hotCount_ = 0;
        selfNode_ = 0;
        lastFullAdmissionMs_ = 0;
        for (size_t i = 0; i < HotCapacity; ++i)
            hot_[i] = {};
        for (size_t i = 0; i < HeardAtCapacity; ++i)
            heardAt_[i] = {};
    }

    // Replace the hot rows after a completed load/reload. Keep the uptime
    // sidecar: the pinned NodeDB load/reload path has no sidecar-clear hook.
    // A newly constructed instance starts with an empty sidecar as on boot.
    bool load(const HotRow *rows, size_t count) {
        if (count > HotCapacity || (count && !rows))
            return false;
        clearHot();
        hotCount_ = count;
        for (size_t i = 0; i < count; ++i)
            hot_[i] = rows[i];
        return true;
    }

    // Clear only hot membership. Stock node reset/removal paths do not
    // explicitly clear the RAM-only heardAt sidecar; use clear() only when
    // resetting this entire volatile mirror (for example, a cold boot).
    void clearHot() {
        hotCount_ = 0;
        for (size_t i = 0; i < HotCapacity; ++i)
            hot_[i] = {};
    }

    // Mirror a completed stock getOrCreateMeshNode creation. Capacity refusal
    // is explicit; the owner must mirror its own selected eviction first.
    bool create(uint32_t node) {
        if (find(node) >= 0)
            return true;
        if (hotCount_ == HotCapacity)
            return false;
        hot_[hotCount_++] = {node, 0, false};
        return true;
    }

    // NodeDB::updateFrom's unknown-ID admission plus getOrCreateMeshNode
    // selection. `isFull` must be the caller's exact NodeDB predicate
    // (capacity OR low heap). The donor checks the 2-second unknown-ID
    // throttle only on that path; its elapsed-time helper treats last=0 as an
    // ordinary timestamp, so an early-boot full admission before uptime 2
    // seconds is deferred as well. Direct owner-driven getOrCreate calls are
    // not subject to that throttle.
    bool createStock(uint32_t node, bool isFull, uint32_t nowMs) {
        return stockRowIndex(node, isFull, nowMs, nullptr) >= 0;
    }

    // Exact decoded-source update path for a future NodeDB-equivalent caller.
    // A new row can be created without a User; later NodeInfo and owner-policy
    // events update only its eviction attributes through setEvictionFlags().
    bool observeStock(uint32_t node, bool hasRxTime, uint32_t stamp, bool viaMqtt,
                      bool isFull, uint32_t nowMs, bool *persistentChanged = nullptr,
                      bool hasObservedFlags = false, uint8_t observedFlags = 0) {
        if (persistentChanged)
            *persistentChanged = false;
        if (hasObservedFlags && (observedFlags & ~uint8_t(KnownEvictionFlags)))
            return false;
        bool created = false;
        const int index = stockRowIndex(node, isFull, nowMs, &created);
        if (index < 0)
            return false;
        HotRow &row = hot_[static_cast<size_t>(index)];
        const uint32_t oldLastHeard = row.lastHeard;
        const bool oldViaMqtt = row.viaMqtt;
        const uint8_t oldFlags = row.evictionFlags;
        if (hasRxTime)
            row.lastHeard = stamp;
        else
            recordHeardWhileClockUntrusted(node, stamp);
        row.viaMqtt = viaMqtt;
        if (hasObservedFlags)
            row.evictionFlags = uint8_t(observedFlags | (row.evictionFlags & uint8_t(HasPublicKey)));
        if (persistentChanged)
            *persistentChanged = created || oldLastHeard != row.lastHeard ||
                oldViaMqtt != row.viaMqtt || oldFlags != row.evictionFlags;
        return true;
    }

    bool setEvictionFlags(uint32_t node, uint8_t flags) {
        if (flags & ~uint8_t(KnownEvictionFlags))
            return false;
        const int index = find(node);
        if (index < 0)
            return false;
        hot_[static_cast<size_t>(index)].evictionFlags = flags;
        return true;
    }

    // Merge an observation from the evictable 16-entry cache. Favorite,
    // ignored, and verification remain current-cache facts, but key presence
    // is logical node metadata: absence after cache eviction/reappearance is
    // not evidence that the public key was explicitly removed. Owner-authorized
    // removal deletes the row; explicit setEvictionFlags() can still clear it.
    bool mergeObservedEvictionFlags(uint32_t node, uint8_t flags) {
        if (flags & ~uint8_t(KnownEvictionFlags))
            return false;
        const int index = find(node);
        if (index < 0)
            return false;
        HotRow &row = hot_[static_cast<size_t>(index)];
        flags |= row.evictionFlags & uint8_t(HasPublicKey);
        if (row.evictionFlags == flags)
            return false;
        row.evictionFlags = flags;
        return true;
    }

    // NodeDB::updateFrom semantics: create if absent, preserve last_heard when
    // rx_time is uptime-only, retain that separate uptime stamp, and always
    // overwrite the latest via_mqtt origin bit.
    bool observe(uint32_t node, bool hasRxTime, uint32_t stamp, bool viaMqtt) {
        // NodeDB::updateFrom admits only packets with a nonzero source ID;
        // zero is also the empty-slot sentinel in the uptime sidecar.
        if (node == 0)
            return false;
        if (find(node) < 0 && !create(node))
            return false;
        const int index = find(node);
        if (index < 0)
            return false;
        HotRow &row = hot_[static_cast<size_t>(index)];
        if (hasRxTime)
            row.lastHeard = stamp;
        else
            recordHeardWhileClockUntrusted(node, stamp);
        row.viaMqtt = viaMqtt;
        return true;
    }

    bool setOrigin(uint32_t node, bool viaMqtt) {
        if (node == 0)
            return false;
        const int index = find(node);
        if (index < 0)
            return false;
        hot_[static_cast<size_t>(index)].viaMqtt = viaMqtt;
        return true;
    }

    // Self is metadata only. Stock's online count does not exclude this node;
    // its row is added/refreshed through the normal hot-row hooks.
    void setSelf(uint32_t node) { selfNode_ = node; }
    uint32_t self() const { return selfNode_; }

    // Mirrors NodeDB::removeNodeByNum (all matching hot rows) and the hot-slot
    // part of selected eviction (one selected slot). Stock does not remove
    // heardAt sidecar records here; they expire during backfill instead.
    size_t remove(uint32_t node) {
        size_t out = 0;
        size_t removed = 0;
        for (size_t i = 0; i < hotCount_; ++i) {
            if (hot_[i].node == node) {
                ++removed;
                continue;
            }
            if (out != i)
                hot_[out] = hot_[i];
            ++out;
        }
        hotCount_ = out;
        for (size_t i = out; i < HotCapacity; ++i)
            hot_[i] = {};
        return removed;
    }

    bool evictAt(size_t index) {
        if (index >= hotCount_)
            return false;
        for (size_t i = index; i + 1 < hotCount_; ++i)
            hot_[i] = hot_[i + 1];
        hot_[--hotCount_] = {};
        return true;
    }

    size_t hotCount() const { return hotCount_; }
    const HotRow *hotAt(size_t index) const { return index < hotCount_ ? &hot_[index] : nullptr; }

    // Literal policy of NodeDB::getNumOnlineMeshNodes(true) and sinceLastSeen:
    // latest MQTT-origin rows are skipped; a signed negative age clamps to 0;
    // only ages strictly below two hours count. No identity/flag filter.
    size_t countOnlineLocal(uint32_t now) const {
        size_t count = 0;
        for (size_t i = 0; i < hotCount_; ++i) {
            const HotRow &row = hot_[i];
            if (row.viaMqtt)
                continue;
            int delta = (int)(now - row.lastHeard);
            if (delta < 0)
                delta = 0;
            if (static_cast<uint32_t>(delta) < OnlineSeconds)
                ++count;
        }
        return count;
    }

    void recordHeardWhileClockUntrusted(uint32_t node, uint32_t uptimeSeconds) {
        if (node == 0)
            return; // zero denotes an unused sidecar slot
        for (size_t i = 0; i < HeardAtCapacity; ++i) {
            if (heardAt_[i].node == node) {
                heardAt_[i].uptimeSeconds = uptimeSeconds;
                return;
            }
        }
        size_t victim = 0;
        for (size_t i = 0; i < HeardAtCapacity; ++i) {
            if (heardAt_[i].node == 0) {
                victim = i;
                break;
            }
            if (heardAt_[i].uptimeSeconds < heardAt_[victim].uptimeSeconds)
                victim = i;
        }
        heardAt_[victim] = {node, uptimeSeconds};
    }

    bool getHeardAtUptimeSeconds(uint32_t node, uint32_t &stamp) const {
        if (node == 0)
            return false;
        for (size_t i = 0; i < HeardAtCapacity; ++i) {
            if (heardAt_[i].node == node) {
                stamp = heardAt_[i].uptimeSeconds;
                return true;
            }
        }
        return false;
    }

    // Mirrors NodeDB::backfillHeardAt. An untrusted clock is a no-op. Once
    // trusted, each nonempty stamp is converted if its hot row still exists,
    // never moves last_heard backward, and is then cleared either way.
    void backfillHeardAt(uint32_t nowEpoch, uint32_t nowUptimeSeconds) {
        if (nowEpoch == 0)
            return;
        for (size_t i = 0; i < HeardAtCapacity; ++i) {
            HeardAt &heard = heardAt_[i];
            if (heard.node == 0)
                continue;
            const int index = find(heard.node);
            if (index >= 0) {
                const uint32_t elapsed = nowUptimeSeconds - heard.uptimeSeconds;
                HotRow &row = hot_[static_cast<size_t>(index)];
                if (elapsed < nowEpoch && nowEpoch - elapsed > row.lastHeard)
                    row.lastHeard = nowEpoch - elapsed;
            }
            heard = {};
        }
    }

private:
    // Returns the selected/created row index so admission and observation can
    // share the one ID lookup. On a victim replacement the new row is always
    // appended after the existing in-place victim compaction.
    int stockRowIndex(uint32_t node, bool isFull, uint32_t nowMs, bool *created) {
        if (created)
            *created = false;
        if (!node || node == selfNode_)
            return -1;
        const int existing = find(node);
        if (existing >= 0)
            return existing;

        const bool full = isFull || hotCount_ >= HotCapacity;
        if (full) {
            if (uint32_t(nowMs - lastFullAdmissionMs_) < FullAdmissionIntervalMs)
                return -1;
            lastFullAdmissionMs_ = nowMs;
            const int victim = stockVictim();
            if (victim >= 0) {
                (void)evictAt(static_cast<size_t>(victim));
            } else if (hotCount_ >= HotCapacity) {
                return -1;
            }
        }
        if (hotCount_ >= HotCapacity)
            return -1;
        const size_t index = hotCount_++;
        hot_[index] = {node, 0, false, 0};
        if (created)
            *created = true;
        return static_cast<int>(index);
    }

    bool heardThisBoot(uint32_t node) const {
        uint32_t ignored = 0;
        return getHeardAtUptimeSeconds(node, ignored);
    }

    bool recencyOlder(const HotRow &candidate, const HotRow &incumbent) const {
        const bool candidateThisBoot = heardThisBoot(candidate.node);
        const bool incumbentThisBoot = heardThisBoot(incumbent.node);
        if (candidateThisBoot != incumbentThisBoot)
            return !candidateThisBoot;
        const uint32_t candidateTime = candidateThisBoot
            ? heardAtUptime(candidate.node) : candidate.lastHeard;
        const uint32_t incumbentTime = incumbentThisBoot
            ? heardAtUptime(incumbent.node) : incumbent.lastHeard;
        return candidateTime < incumbentTime;
    }

    uint32_t heardAtUptime(uint32_t node) const {
        uint32_t stamp = 0;
        (void)getHeardAtUptimeSeconds(node, stamp);
        return stamp;
    }

    int stockVictim() const {
        // Stock reserves row zero for self and keeps first-row order on ties.
        int oldest = -1;
        int oldestKeyless = -1;
        for (size_t i = 1; i < hotCount_; ++i) {
            const HotRow &row = hot_[i];
            const bool favorite = row.evictionFlags & Favorite;
            const bool ignored = row.evictionFlags & Ignored;
            const bool verified = row.evictionFlags & ManuallyVerified;
            if (!favorite && !ignored && !verified &&
                (oldest < 0 || recencyOlder(row, hot_[static_cast<size_t>(oldest)])))
                oldest = static_cast<int>(i);
            // Match donor literally: this secondary preference does not test
            // ManuallyVerified; stock's keyless row is selected if present.
            if (!favorite && !ignored && !(row.evictionFlags & HasPublicKey) &&
                (oldestKeyless < 0 || recencyOlder(row, hot_[static_cast<size_t>(oldestKeyless)])))
                oldestKeyless = static_cast<int>(i);
        }
        return oldestKeyless >= 0 ? oldestKeyless : oldest;
    }

    int find(uint32_t node) const {
        for (size_t i = 0; i < hotCount_; ++i)
            if (hot_[i].node == node)
                return static_cast<int>(i);
        return -1;
    }

    HotRow hot_[HotCapacity]{};
    HeardAt heardAt_[HeardAtCapacity]{};
    size_t hotCount_ = 0;
    uint32_t selfNode_ = 0;
    uint32_t lastFullAdmissionMs_ = 0;
};

} // namespace mqtt
} // namespace mt
