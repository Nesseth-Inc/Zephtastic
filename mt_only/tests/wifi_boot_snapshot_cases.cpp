#include "network/WifiBootSnapshot.h"
#include <assert.h>
#include <string.h>

// Bounded fixture for the locked WifiBootSnapshot API.
// Synthetic strings only: SsidA, PskA, SsidBB, PskBBB.
// Compares observable state, pointers, and strings only; never memcmps
// class padding. WifiBootSnapshot is noncopyable/nonmovable by contract;
// this fixture never copies or moves a snapshot.

using namespace mt::network;

// Fill dst with dstLen characters drawn from src; NUL-terminate at dst[dstLen].
static void fillFrom(char *dst, size_t dstLen, const char *src) {
    const size_t n = strlen(src);
    for (size_t i = 0; i < dstLen; ++i) dst[i] = src[i % n];
    dst[dstLen] = '\0';
}

static void caseUnpreparedAccessors() {
    WifiBootSnapshot snap;
    assert(!snap.prepared());
    assert(!snap.active());
    assert(snap.ssid() != nullptr);
    assert(snap.psk() != nullptr);
    assert(snap.ssid() == snap.ssid());
    assert(snap.psk() == snap.psk());
    assert(strcmp(snap.ssid(), "") == 0);
    assert(strcmp(snap.psk(), "") == 0);
}

static void caseTamperedSealRejectedWithoutStateChange() {
    WifiStationSettings rec;
    rec.set(true, "SsidA", 5, "PskA", 4);
    assert(rec.valid());
    assert(rec.enabled());
    assert(rec.ssidLength() == 5);
    assert(rec.pskLength() == 4);

    rec.bytes[106] ^= 0xFF;  // break the CRC seal (bytes 106..107)
    assert(!rec.valid());

    WifiBootSnapshot snap;
    assert(!snap.prepare(rec, true));
    assert(!snap.prepared());
    assert(!snap.active());
    assert(strcmp(snap.ssid(), "") == 0);
    assert(strcmp(snap.psk(), "") == 0);

    // No state change: a valid record is still accepted afterwards.
    WifiStationSettings good;
    good.set(true, "SsidA", 5, "PskA", 4);
    assert(good.valid());
    assert(snap.prepare(good, true));
    assert(snap.prepared());
    assert(snap.active());
    assert(strcmp(snap.ssid(), "SsidA") == 0);
    assert(strcmp(snap.psk(), "PskA") == 0);
}

static void caseEnabledIncapableRejectedWithoutStateChange() {
    WifiStationSettings rec;
    rec.set(true, "SsidA", 5, "PskA", 4);
    assert(rec.valid());

    WifiBootSnapshot snap;
    assert(!snap.prepare(rec, false));
    assert(!snap.prepared());
    assert(!snap.active());
    assert(strcmp(snap.ssid(), "") == 0);
    assert(strcmp(snap.psk(), "") == 0);

    // No state change: the same record is accepted on a capable board.
    assert(snap.prepare(rec, true));
    assert(snap.prepared());
    assert(snap.active());
    assert(strcmp(snap.ssid(), "SsidA") == 0);
    assert(strcmp(snap.psk(), "PskA") == 0);
}

static void caseDisabledSucceedsInactiveRegardlessOfCapability() {
    WifiStationSettings offIncapable;
    offIncapable.set(false, "SsidA", 5, "PskA", 4);
    assert(offIncapable.valid());
    assert(!offIncapable.enabled());

    WifiBootSnapshot snapIncapable;
    assert(snapIncapable.prepare(offIncapable, false));
    assert(snapIncapable.prepared());
    assert(!snapIncapable.active());
    assert(strcmp(snapIncapable.ssid(), "") == 0);
    assert(strcmp(snapIncapable.psk(), "") == 0);

    WifiStationSettings offCapable;
    offCapable.set(false, "SsidBB", 6, "PskBBB", 6);
    assert(offCapable.valid());
    assert(!offCapable.enabled());

    WifiBootSnapshot snapCapable;
    assert(snapCapable.prepare(offCapable, true));
    assert(snapCapable.prepared());
    assert(!snapCapable.active());
    assert(strcmp(snapCapable.ssid(), "") == 0);
    assert(strcmp(snapCapable.psk(), "") == 0);
}

static void caseBoundsAndCopyOnSuccess() {
    char ssid33[34]; fillFrom(ssid33, 33, "SsidA");
    char psk65[66];  fillFrom(psk65, 65, "PskA");
    char ssid32[33]; fillFrom(ssid32, 32, "SsidA");
    char psk64[65];  fillFrom(psk64, 64, "PskA");

    WifiStationSettings rec;
    rec.set(true, "SsidA", 5, "PskA", 4);
    assert(rec.valid());
    assert(rec.ssidLength() == 5);
    assert(rec.pskLength() == 4);

    // Over-length SSID rejected; record unchanged (copy-on-success).
    rec.set(true, ssid33, 33, "PskA", 4);
    assert(rec.valid());
    assert(rec.ssidLength() == 5);
    assert(rec.pskLength() == 4);

    // Over-length PSK rejected; record unchanged.
    rec.set(true, "SsidA", 5, psk65, 65);
    assert(rec.valid());
    assert(rec.ssidLength() == 5);
    assert(rec.pskLength() == 4);

    // Valid boundaries: exactly 32-byte SSID and 64-byte PSK accepted.
    rec.set(true, ssid32, 32, psk64, 64);
    assert(rec.valid());
    assert(rec.ssidLength() == 32);
    assert(rec.pskLength() == 64);

    WifiBootSnapshot snap;
    assert(snap.prepare(rec, true));
    assert(snap.prepared());
    assert(snap.active());
    assert(snap.ssid() == snap.ssid());
    assert(snap.psk() == snap.psk());
    assert(strcmp(snap.ssid(), ssid32) == 0);
    assert(strlen(snap.ssid()) == 32);
    assert(strcmp(snap.psk(), psk64) == 0);
    assert(strlen(snap.psk()) == 64);
}

static void caseEmptyPsk() {
    WifiStationSettings rec;
    rec.set(true, "SsidA", 5, "", 0);
    assert(rec.valid());
    assert(rec.enabled());
    assert(rec.ssidLength() == 5);
    assert(rec.pskLength() == 0);

    WifiBootSnapshot snap;
    assert(snap.prepare(rec, true));
    assert(snap.prepared());
    assert(snap.active());
    assert(strcmp(snap.ssid(), "SsidA") == 0);
    assert(strlen(snap.ssid()) == 5);
    assert(strcmp(snap.psk(), "") == 0);
    assert(strlen(snap.psk()) == 0);
}

static void caseABObjectOwnedCopies() {
    WifiStationSettings recA;
    recA.set(true, "SsidA", 5, "PskA", 4);
    assert(recA.valid());

    WifiBootSnapshot snapA;
    assert(snapA.prepare(recA, true));
    assert(snapA.prepared());
    assert(snapA.active());
    const char *ssidA = snapA.ssid();
    const char *pskA = snapA.psk();
    assert(strcmp(ssidA, "SsidA") == 0);
    assert(strlen(ssidA) == 5);
    assert(strcmp(pskA, "PskA") == 0);
    assert(strlen(pskA) == 4);

    WifiStationSettings recB;
    recB.set(true, "SsidBB", 6, "PskBBB", 6);
    assert(recB.valid());

    WifiBootSnapshot snapB;
    assert(snapB.prepare(recB, true));
    assert(snapB.prepared());
    assert(snapB.active());

    // Distinct object-owned buffers with correct NUL-terminated content.
    assert(snapB.ssid() != ssidA);
    assert(snapB.psk() != pskA);
    assert(strcmp(snapB.ssid(), "SsidBB") == 0);
    assert(strlen(snapB.ssid()) == 6);
    assert(strcmp(snapB.psk(), "PskBBB") == 0);
    assert(strlen(snapB.psk()) == 6);

    // Editing the original record after acceptance does not disturb the
    // accepted snapshot: pointers and values stay stable.
    recA.set(true, "SsidBB", 6, "PskBBB", 6);
    assert(snapA.ssid() == ssidA);
    assert(snapA.psk() == pskA);
    assert(strcmp(snapA.ssid(), "SsidA") == 0);
    assert(strcmp(snapA.psk(), "PskA") == 0);
    assert(strcmp(snapB.ssid(), "SsidBB") == 0);
    assert(strcmp(snapB.psk(), "PskBBB") == 0);
}

static void caseOneShotKeepsAcceptedSnapshot() {
    WifiStationSettings recA;
    recA.set(true, "SsidA", 5, "PskA", 4);
    assert(recA.valid());
    WifiStationSettings recB;
    recB.set(true, "SsidBB", 6, "PskBBB", 6);
    assert(recB.valid());

    WifiBootSnapshot snap;
    assert(snap.prepare(recA, true));
    const char *ssidA = snap.ssid();
    const char *pskA = snap.psk();
    assert(strcmp(ssidA, "SsidA") == 0);
    assert(strcmp(pskA, "PskA") == 0);

    // One-shot: a different record is rejected, snapshot unchanged.
    assert(!snap.prepare(recB, true));
    assert(snap.prepared());
    assert(snap.active());
    assert(snap.ssid() == ssidA);
    assert(snap.psk() == pskA);
    assert(strcmp(snap.ssid(), "SsidA") == 0);
    assert(strcmp(snap.psk(), "PskA") == 0);

    // One-shot: even the identical record is rejected.
    assert(!snap.prepare(recA, true));
    assert(snap.ssid() == ssidA);
    assert(snap.psk() == pskA);
    assert(strcmp(snap.ssid(), "SsidA") == 0);
    assert(strcmp(snap.psk(), "PskA") == 0);
}

static void runCases() {
    caseUnpreparedAccessors();
    caseTamperedSealRejectedWithoutStateChange();
    caseEnabledIncapableRejectedWithoutStateChange();
    caseDisabledSucceedsInactiveRegardlessOfCapability();
    caseBoundsAndCopyOnSuccess();
    caseEmptyPsk();
    caseABObjectOwnedCopies();
    caseOneShotKeepsAcceptedSnapshot();
}

int main() { runCases(); return 0; }
