// SPDX-License-Identifier: GPL-3.0-only
// Pure native oracle tests for the pinned stock NodeDB online-local policy.
#include "mqtt/OnlineNodeWindow.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using Window = mt::mqtt::OnlineNodeWindow;

// Literal oracle from pinned stock Meshtastic 2.8.1 NodeDB.cpp:
//   int delta = (int)(getTime() - n->last_heard);
//   if (delta < 0) delta = 0;
//   return delta;
// and getNumOnlineMeshNodes(true) skips via-MQTT rows then accepts
// sinceLastSeen(row) < NUM_ONLINE_SECS (60*60*2).
static uint32_t stockSinceLastSeen(uint32_t now, uint32_t lastHeard) {
    int delta = (int)(now - lastHeard);
    if (delta < 0)
        delta = 0;
    return static_cast<uint32_t>(delta);
}

static size_t stockOnlineLocal(const std::vector<Window::HotRow> &rows, uint32_t now) {
    size_t count = 0;
    for (const auto &row : rows) {
        if (row.viaMqtt)
            continue;
        if (stockSinceLastSeen(now, row.lastHeard) < 60u * 60u * 2u)
            ++count;
    }
    return count;
}

static void pass(const char *name) { std::cout << "PASS " << name << '\n'; }

static std::vector<Window::HotRow> rows(const Window &window) {
    std::vector<Window::HotRow> out;
    for (size_t i = 0; i < window.hotCount(); ++i)
        out.push_back(*window.hotAt(i));
    return out;
}

// Test-only differential oracle, written independently of OnlineNodeWindow's
// victim helpers from the pinned NodeDB.cpp candidate loop.
static bool oracleOlder(const Window &window, const Window::HotRow &a,
                        const Window::HotRow &b) {
    uint32_t aStamp = 0, bStamp = 0;
    const bool aBoot = window.getHeardAtUptimeSeconds(a.node, aStamp);
    const bool bBoot = window.getHeardAtUptimeSeconds(b.node, bStamp);
    if (aBoot != bBoot)
        return !aBoot;
    return (aBoot ? aStamp : a.lastHeard) < (bBoot ? bStamp : b.lastHeard);
}

static int stockVictimOracle(const Window &window) {
    int oldest = -1;
    int oldestKeyless = -1;
    for (size_t i = 1; i < window.hotCount(); ++i) {
        const auto &row = *window.hotAt(i);
        const bool favorite = row.evictionFlags & Window::Favorite;
        const bool ignored = row.evictionFlags & Window::Ignored;
        const bool verified = row.evictionFlags & Window::ManuallyVerified;
        if (!favorite && !ignored && !verified &&
            (oldest < 0 || oracleOlder(window, row, *window.hotAt(static_cast<size_t>(oldest)))))
            oldest = static_cast<int>(i);
        // The pinned stock secondary keyless scan intentionally omits verified.
        if (!favorite && !ignored && !(row.evictionFlags & Window::HasPublicKey) &&
            (oldestKeyless < 0 || oracleOlder(window, row, *window.hotAt(static_cast<size_t>(oldestKeyless)))))
            oldestKeyless = static_cast<int>(i);
    }
    return oldestKeyless >= 0 ? oldestKeyless : oldest;
}

static Window fullStockWindow() {
    Window window;
    Window::HotRow input[Window::HotCapacity]{};
    input[0] = {0x100, 1000, false, 0};
    for (size_t i = 1; i < Window::HotCapacity; ++i)
        input[i] = {static_cast<uint32_t>(0x100 + i), static_cast<uint32_t>(1000 + i), false,
                    Window::HasPublicKey};
    assert(window.load(input, Window::HotCapacity));
    window.setSelf(0x100);
    return window;
}

static void stockVictimMatchesOracleAndStableTies() {
    Window w = fullStockWindow();
    // Equal epoch timestamps preserve first-row order; self row zero is never
    // considered. Protected rows cannot become the ordinary victim.
    assert(w.setEvictionFlags(0x101, Window::Favorite | Window::HasPublicKey));
    assert(w.setEvictionFlags(0x102, Window::Ignored | Window::HasPublicKey));
    assert(w.setEvictionFlags(0x103, Window::ManuallyVerified | Window::HasPublicKey));
    assert(w.setEvictionFlags(0x104, Window::Favorite | Window::Ignored | Window::HasPublicKey));
    for (size_t i = 5; i < Window::HotCapacity; ++i)
        assert(w.setEvictionFlags(static_cast<uint32_t>(0x100 + i), Window::HasPublicKey));
    // Give the first unprotected rows equal recency; oracle chooses row 5.
    assert(w.observeStock(0x105, true, 50, false, false, 9000));
    assert(w.observeStock(0x106, true, 50, false, false, 9000));
    const int expected = stockVictimOracle(w);
    assert(expected == 5);
    const uint32_t victimNode = w.hotAt(static_cast<size_t>(expected))->node;
    assert(w.createStock(0x500, true, 2000));
    assert(w.hotCount() == Window::HotCapacity);
    assert(w.hotAt(Window::HotCapacity - 1)->node == 0x500);
    for (size_t i = 0; i < w.hotCount(); ++i)
        assert(w.hotAt(i)->node != victimNode);
    assert(w.hotAt(0)->node == 0x100);
    pass("stock-victim-protection-self-row-and-stable-first-tie");
}

static void logicalPublicKeySurvivesHotCacheAbsenceAndKeylessReappearance() {
    Window w = fullStockWindow();
    assert(w.setEvictionFlags(0x101, Window::Favorite | Window::HasPublicKey));
    assert(w.setEvictionFlags(0x102, 0));
    assert(w.observeStock(0x101, true, 10, false, false, 9000));
    assert(w.observeStock(0x102, true, 100, false, false, 9000));

    // The 16-entry NodeCache may evict this row and later recreate it from a
    // keyless NodeInfo. Automatic synchronization clears current policy bits,
    // but key absence is not an explicit key-removal operation.
    assert(w.mergeObservedEvictionFlags(0x101, 0));
    assert(w.hotAt(1)->evictionFlags == Window::HasPublicKey);
    const int expected = stockVictimOracle(w);
    assert(expected == 2); // keyless row 0x102 outranks older keyed row 0x101
    assert(w.createStock(0x500, true, 2000));
    assert(w.hotAt(Window::HotCapacity - 1)->node == 0x500);
    for (size_t i = 0; i < w.hotCount(); ++i)
        assert(w.hotAt(i)->node != 0x102);
    assert(w.hotAt(1)->node == 0x101);

    // An explicit logical-node removal drops all old metadata. A later
    // keyless baseline therefore starts keyless; owner-set synchronization
    // remains able to clear the bit explicitly as well.
    Window removed;
    assert(removed.create(0x200));
    assert(removed.create(0x201));
    assert(removed.setEvictionFlags(0x201, Window::HasPublicKey));
    assert(removed.remove(0x201) == 1);
    assert(removed.create(0x201));
    assert(removed.hotAt(1)->evictionFlags == 0);
    assert(!removed.mergeObservedEvictionFlags(0x201, 0));
    assert(removed.setEvictionFlags(0x201, Window::HasPublicKey));
    assert(removed.setEvictionFlags(0x201, 0));
    assert(removed.hotAt(1)->evictionFlags == 0);
    pass("logical-public-key-preserved-until-explicit-remove-or-clear");
}

static void keylessAndSidecarRecencyMatchOracle() {
    Window w = fullStockWindow();
    // Keyless rows outrank keyed rows even when their epoch timestamp is newer.
    assert(w.setEvictionFlags(0x101, Window::HasPublicKey));
    assert(w.setEvictionFlags(0x102, 0));
    assert(w.observeStock(0x101, true, 1, false, false, 9000));
    assert(w.observeStock(0x102, true, 900, false, false, 9000));
    int expected = stockVictimOracle(w);
    assert(expected == 2);
    const uint32_t keylessVictim = w.hotAt(static_cast<size_t>(expected))->node;
    assert(w.createStock(0x500, true, 2000));
    assert(w.hotAt(Window::HotCapacity - 1)->node == 0x500);
    for (size_t i = 0; i < w.hotCount(); ++i)
        assert(w.hotAt(i)->node != keylessVictim);

    Window u = fullStockWindow();
    // Any current-boot sidecar row is newer than rows without one. Sidecar
    // values then determine age, with the first equal stamp retaining priority.
    assert(u.setEvictionFlags(0x101, Window::HasPublicKey));
    assert(u.setEvictionFlags(0x102, Window::HasPublicKey));
    u.recordHeardWhileClockUntrusted(0x101, 10);
    u.recordHeardWhileClockUntrusted(0x102, 10);
    assert(u.setEvictionFlags(0x103, Window::Favorite | Window::HasPublicKey));
    expected = stockVictimOracle(u);
    assert(expected == 4); // no-sidecar, oldest eligible row
    const uint32_t sidecarVictim = u.hotAt(static_cast<size_t>(expected))->node;
    assert(u.createStock(0x501, true, 2000));
    assert(u.hotAt(Window::HotCapacity - 1)->node == 0x501);
    for (size_t i = 0; i < u.hotCount(); ++i)
        assert(u.hotAt(i)->node != sidecarVictim);

    Window v = fullStockWindow();
    assert(v.setEvictionFlags(0x101, 0));
    assert(v.setEvictionFlags(0x102, 0));
    v.recordHeardWhileClockUntrusted(0x101, 20);
    v.recordHeardWhileClockUntrusted(0x102, 10);
    expected = stockVictimOracle(v);
    assert(expected == 2);
    const uint32_t earliestSidecarVictim = v.hotAt(static_cast<size_t>(expected))->node;
    assert(v.createStock(0x502, true, 2000));
    assert(v.hotAt(Window::HotCapacity - 1)->node == 0x502);
    for (size_t i = 0; i < v.hotCount(); ++i)
        assert(v.hotAt(i)->node != earliestSidecarVictim);
    pass("stock-keyless-priority-sidecar-recency-and-sidecar-stable-ties");
}

static void stockAdmissionThrottleAndProtectedRefusal() {
    Window w = fullStockWindow();
    for (size_t i = 1; i < Window::HotCapacity; ++i)
        assert(w.setEvictionFlags(static_cast<uint32_t>(0x100 + i),
                                  Window::Favorite | Window::HasPublicKey));
    const auto before = rows(w);
    assert(!w.createStock(0x500, true, 1999));
    assert(rows(w).size() == before.size() && w.hotAt(1)->node == before[1].node);
    assert(!w.createStock(0x500, true, 2000)); // exact boundary is admitted to selection
    // No victim and no space means no row is published. The throttle timestamp
    // still advances before selection, as in updateFrom's caller.
    assert(w.hotCount() == Window::HotCapacity && w.hotAt(1)->node != 0x500);
    assert(!w.createStock(0x500, true, 3999));
    assert(!w.createStock(0x500, true, 4000));
    pass("full-admission-throttle-protected-full-refusal-and-throttle-on-refusal");
}

static void lowHeapPathEvictsBelowCapacityAndExistingIDsBypassThrottle() {
    Window w;
    Window::HotRow input[]{{0x100, 1000, false, 0},
                           {0x101, 10, false, Window::HasPublicKey},
                           {0x102, 20, false, Window::HasPublicKey}};
    assert(w.load(input, 3));
    w.setSelf(0x100);
    // `isFull` is true due low heap, so stock performs a victim scan even
    // below the row-count limit.
    assert(w.createStock(0x500, true, 2000));
    assert(w.hotCount() == 3 && w.hotAt(1)->node == 0x102 && w.hotAt(2)->node == 0x500);
    // Known IDs are not subject to the full unknown-ID admission throttle.
    assert(w.observeStock(0x102, true, 30, true, true, 2001));
    assert(w.hotCount() == 3 && w.hotAt(1)->lastHeard == 30 && w.hotAt(1)->viaMqtt);
    assert(w.createStock(0x500, true, 2001));
    assert(w.hotCount() == 3);

    Window append;
    Window::HotRow protectedRows[]{{0x200, 10, false, 0},
                                   {0x201, 20, false, Window::Favorite | Window::HasPublicKey},
                                   {0x202, 30, false, Window::Ignored | Window::HasPublicKey}};
    assert(append.load(protectedRows, 3));
    append.setSelf(0x200);
    // Stock still appends below the hard row capacity if low-heap is the only
    // reason isFull() is true and no removable row exists.
    assert(append.createStock(0x203, true, 2000));
    assert(append.hotCount() == 4 && append.hotAt(3)->node == 0x203);

    Window verifiedKeyless = fullStockWindow();
    assert(verifiedKeyless.setEvictionFlags(0x101,
                                           Window::ManuallyVerified | Window::Favorite));
    assert(verifiedKeyless.setEvictionFlags(0x102, Window::ManuallyVerified));
    const int verifiedExpected = stockVictimOracle(verifiedKeyless);
    assert(verifiedExpected == 2); // keyless secondary selector omits verified
    assert(verifiedKeyless.createStock(0x504, true, 2000));
    assert(verifiedKeyless.hotAt(Window::HotCapacity - 1)->node == 0x504);
    for (size_t i = 0; i < verifiedKeyless.hotCount(); ++i)
        assert(verifiedKeyless.hotAt(i)->node != 0x102);

    Window invalidFlags;
    assert(invalidFlags.create(0x301));
    assert(!invalidFlags.setEvictionFlags(0x301, 0x80));
    assert(invalidFlags.hotAt(0)->evictionFlags == 0);
    pass("low-heap-full-predicate-and-known-id-throttle-bypass");
}

static void donorLiteralCountBoundariesAndUnfilteredRows() {
    // Stock includes every hot row, including self, ignored-user, and an
    // id-zero placeholder if it is within numMeshNodes; only via_mqtt and age
    // affect getNumOnlineMeshNodes(true).
    const std::vector<Window::HotRow> input{{0x10, 10000, false},  // self
                                             {0x11, 9999, false},  // ignored flag is irrelevant
                                             {0, 9999, false},     // placeholder row
                                             {0x12, 10001, false}, // future timestamp clamps age to 0
                                             {0x13, 2800, false},  // exactly 7200 seconds: offline
                                             {0x14, 2801, false},  // 7199 seconds: online
                                             {0x15, 9999, true}};  // latest origin is MQTT
    Window window;
    assert(window.load(input.data(), input.size()));
    window.setSelf(0x10);
    assert(window.self() == 0x10);
    assert(window.countOnlineLocal(10000) == stockOnlineLocal(input, 10000));
    assert(window.countOnlineLocal(10000) == 5);

    Window zeroEpoch;
    const Window::HotRow zeroRow{7, 0, false};
    assert(zeroEpoch.load(&zeroRow, 1));
    assert(zeroEpoch.countOnlineLocal(7199) == stockOnlineLocal(rows(zeroEpoch), 7199));
    assert(zeroEpoch.countOnlineLocal(7199) == 1);
    assert(zeroEpoch.countOnlineLocal(7200) == 0);

    // Unsigned subtraction followed by the donor's signed clamp handles wrap
    // and future stamps exactly as the pinned NodeDB implementation.
    const Window::HotRow wrapped{8, 9990, false};
    const Window::HotRow future{9, 10010, false};
    const Window::HotRow aged{10, 10000 - 7200, false};
    const Window::HotRow mqtt{11, 9999, true};
    const Window::HotRow boundaryRows[]{wrapped, future, aged, mqtt};
    Window boundary;
    assert(boundary.load(boundaryRows, 4));
    assert(boundary.countOnlineLocal(10000) == stockOnlineLocal(rows(boundary), 10000));
    assert(boundary.countOnlineLocal(10000) == 2);
    const Window::HotRow uptimeWrap{12, std::numeric_limits<uint32_t>::max() - 5, false};
    Window wrap;
    assert(wrap.load(&uptimeWrap, 1));
    assert(wrap.countOnlineLocal(10) == stockOnlineLocal(rows(wrap), 10));
    assert(wrap.countOnlineLocal(10) == 1);
    pass("literal-stock-count-includes-self-ignored-placeholder-future-time-and-strict-two-hour-boundary");
}

static void hotLifecycleAndLatestOrigin() {
    Window w;
    assert(w.create(1));
    assert(w.hotCount() == 1 && w.hotAt(0)->lastHeard == 0 && !w.hotAt(0)->viaMqtt);
    assert(w.create(1) && w.hotCount() == 1); // getOrCreate is idempotent
    assert(w.observe(1, true, 1000, true));
    assert(w.countOnlineLocal(1001) == 0);
    assert(w.observe(1, true, 1002, false));
    assert(w.countOnlineLocal(1003) == 1);
    assert(w.setOrigin(1, true) && w.countOnlineLocal(1003) == 0);
    assert(w.setOrigin(1, false) && w.countOnlineLocal(1003) == 1);

    Window::HotRow duplicateRows[]{{2, 1000, false}, {2, 900, true}, {3, 800, false}};
    assert(w.load(duplicateRows, 3));
    assert(w.countOnlineLocal(1001) == 2); // count is per hot row, not unique node ID
    assert(w.remove(2) == 2 && w.hotCount() == 1 && w.hotAt(0)->node == 3);

    Window::HotRow ordered[]{{20, 100, false}, {21, 200, false}, {22, 300, false}};
    assert(w.load(ordered, 3));
    assert(w.evictAt(1) && w.hotCount() == 2 && w.hotAt(0)->node == 20 && w.hotAt(1)->node == 22);
    assert(!w.evictAt(2));
    pass("hot-load-create-remove-selected-eviction-duplicates-and-origin-hooks");
}

static void stockObservationReportsPersistentChanges() {
    Window window;
    assert(window.create(0x100));
    bool changed = false;
    assert(window.observeStock(0x101, true, 400, false, false, 9000, &changed));
    assert(changed && window.hotCount() == 2 && window.hotAt(1)->lastHeard == 400);

    assert(window.observeStock(0x101, true, 400, false, false, 9001, &changed));
    assert(!changed); // identical persistent state must not dirty the journal
    assert(window.observeStock(0x101, true, 401, false, false, 9002, &changed));
    assert(changed && window.hotAt(1)->lastHeard == 401);
    assert(window.observeStock(0x101, true, 401, true, false, 9003, &changed));
    assert(changed && window.hotAt(1)->viaMqtt);

    const uint8_t keyedFavorite = Window::HasPublicKey | Window::Favorite;
    assert(window.observeStock(0x101, true, 401, true, false, 9004, &changed,
                               true, keyedFavorite));
    assert(changed && window.hotAt(1)->evictionFlags == keyedFavorite);
    assert(window.observeStock(0x101, true, 401, true, false, 9005, &changed,
                               true, keyedFavorite));
    assert(!changed);
    // A cache-recreated keyless entry refreshes volatile policy but cannot
    // clear the logical key-presence bit in the hot metadata row.
    assert(window.observeStock(0x101, true, 401, true, false, 9006, &changed,
                               true, Window::Ignored));
    assert(changed && window.hotAt(1)->evictionFlags ==
           (Window::HasPublicKey | Window::Ignored));
    assert(window.observeStock(0x101, true, 401, true, false, 9007, &changed));
    assert(!changed && window.hotAt(1)->evictionFlags ==
           (Window::HasPublicKey | Window::Ignored));

    uint32_t sidecar = 0;
    assert(window.observeStock(0x101, false, 777, true, false, 9008, &changed));
    assert(!changed && window.hotAt(1)->lastHeard == 401);
    assert(window.getHeardAtUptimeSeconds(0x101, sidecar) && sidecar == 777);
    assert(window.observeStock(0x102, false, 888, false, false, 9009, &changed));
    assert(changed && window.hotAt(2)->lastHeard == 0);
    assert(window.getHeardAtUptimeSeconds(0x102, sidecar) && sidecar == 888);

    const auto before = rows(window);
    changed = true;
    assert(!window.observeStock(0x102, true, 900, false, false, 9010, &changed,
                                true, 0x80));
    assert(!changed && rows(window).size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
        assert(window.hotAt(i)->node == before[i].node &&
               window.hotAt(i)->lastHeard == before[i].lastHeard &&
               window.hotAt(i)->viaMqtt == before[i].viaMqtt &&
               window.hotAt(i)->evictionFlags == before[i].evictionFlags);

    Window throttled = fullStockWindow();
    const auto fullBefore = rows(throttled);
    changed = true;
    assert(!throttled.observeStock(0x500, false, 999, false, true, 1999,
                                   &changed));
    assert(!changed && rows(throttled).size() == fullBefore.size());
    for (size_t i = 0; i < fullBefore.size(); ++i)
        assert(throttled.hotAt(i)->node == fullBefore[i].node &&
               throttled.hotAt(i)->lastHeard == fullBefore[i].lastHeard &&
               throttled.hotAt(i)->evictionFlags == fullBefore[i].evictionFlags);
    pass("stock-observation-single-pass-change-result-cache-key-merge-and-sidecar-semantics");
}

static void uptimeSidecarAndBackfill() {
    Window w;
    assert(w.observe(100, false, 100, false));
    assert(w.hotAt(0)->lastHeard == 0);
    uint32_t stamp = 0;
    assert(w.getHeardAtUptimeSeconds(100, stamp) && stamp == 100);
    w.backfillHeardAt(0, 200); // stock leaves sidecar untouched until clock trusted
    assert(w.getHeardAtUptimeSeconds(100, stamp) && stamp == 100);
    w.backfillHeardAt(30000, 110);
    assert(w.hotAt(0)->lastHeard == 29990);
    assert(!w.getHeardAtUptimeSeconds(100, stamp));

    // An epoch observation newer than the sidecar-derived candidate wins.
    assert(w.observe(100, false, 120, false));
    assert(w.observe(100, true, 49990, false));
    w.backfillHeardAt(50000, 200);
    assert(w.hotAt(0)->lastHeard == 49990);
    assert(!w.getHeardAtUptimeSeconds(100, stamp));

    assert(w.observe(101, false, 500, false));
    assert(w.remove(101) == 1);
    assert(w.getHeardAtUptimeSeconds(101, stamp) && stamp == 500); // stock removal leaves RAM sidecar
    w.backfillHeardAt(40000, 600); // absent hot row: discard stamp without creating a row
    assert(!w.getHeardAtUptimeSeconds(101, stamp));

    Window full;
    for (uint32_t i = 1; i <= Window::HeardAtCapacity; ++i)
        full.recordHeardWhileClockUntrusted(i, i * 10);
    full.recordHeardWhileClockUntrusted(33, 1); // oldest stamp is replaced
    assert(!full.getHeardAtUptimeSeconds(1, stamp));
    assert(full.getHeardAtUptimeSeconds(2, stamp) && stamp == 20);
    assert(full.getHeardAtUptimeSeconds(33, stamp) && stamp == 1);

    Window reload;
    reload.recordHeardWhileClockUntrusted(200, 50);
    const Window::HotRow loaded{200, 0, false};
    assert(reload.load(&loaded, 1));
    assert(reload.getHeardAtUptimeSeconds(200, stamp) && stamp == 50);
    reload.backfillHeardAt(10000, 55);
    assert(reload.hotAt(0)->lastHeard == 9995);
    pass("stock-32-entry-uptime-sidecar-update-oldest-replacement-and-trusted-clock-backfill");
}

static void zeroNodeObservationIsRejectedWithoutMutation() {
    Window w;
    const Window::HotRow rowsBefore[]{{0, 700, false}, {42, 600, true}};
    assert(w.load(rowsBefore, 2));
    const auto before = rows(w);

    // Stock NodeDB::updateFrom ignores source ID zero, and the sidecar uses
    // zero as its empty-record sentinel. Neither API may publish that ID.
    assert(!w.observe(0, false, 1234, false));
    w.recordHeardWhileClockUntrusted(0, 1234);
    uint32_t stamp = 0xabcdef01u;
    assert(!w.getHeardAtUptimeSeconds(0, stamp) && stamp == 0xabcdef01u);
    assert(!w.setOrigin(0, true));

    assert(w.hotCount() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        assert(w.hotAt(i)->node == before[i].node && w.hotAt(i)->lastHeard == before[i].lastHeard &&
               w.hotAt(i)->viaMqtt == before[i].viaMqtt);
    }
    assert(w.observe(42, false, 900, false));
    assert(w.getHeardAtUptimeSeconds(42, stamp) && stamp == 900);
    pass("zero-node-observation-and-sidecar-sentinel-refuse-without-mutation");
}

static void boundsAndAtomicLoadRefusal() {
    Window w;
    for (uint32_t i = 1; i <= Window::HotCapacity; ++i)
        assert(w.create(i));
    assert(w.hotCount() == Window::HotCapacity);
    assert(!w.create(121)); // caller must mirror stock's selected eviction first
    assert(w.evictAt(0) && w.create(121) && w.hotCount() == Window::HotCapacity);
    const auto before = rows(w);
    assert(!w.load(nullptr, Window::HotCapacity + 1));
    assert(rows(w).size() == before.size());
    for (size_t i = 0; i < before.size(); ++i) {
        assert(w.hotAt(i)->node == before[i].node && w.hotAt(i)->lastHeard == before[i].lastHeard &&
               w.hotAt(i)->viaMqtt == before[i].viaMqtt);
    }
    pass("120-hot-row-bound-capacity-refusal-and-invalid-load-preserves-existing-window");
}

int main() {
    donorLiteralCountBoundariesAndUnfilteredRows();
    hotLifecycleAndLatestOrigin();
    stockObservationReportsPersistentChanges();
    uptimeSidecarAndBackfill();
    zeroNodeObservationIsRejectedWithoutMutation();
    boundsAndAtomicLoadRefusal();
    stockVictimMatchesOracleAndStableTies();
    logicalPublicKeySurvivesHotCacheAbsenceAndKeylessReappearance();
    keylessAndSidecarRecencyMatchOracle();
    stockAdmissionThrottleAndProtectedRefusal();
    lowHeapPathEvictsBelowCapacityAndExistingIDsBypassThrottle();
}
