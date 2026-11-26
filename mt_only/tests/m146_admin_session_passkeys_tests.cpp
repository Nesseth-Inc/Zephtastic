#include "../src/admin/AdminSessionPasskeys.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using mt::admin::AdminSessionPasskeys;

struct EntropyState {
    unsigned calls = 0;
    bool fail = false;
    bool allZero = false;
    uint8_t next = 1;
};

static bool fakeEntropy(void *context, uint8_t *output, size_t length) {
    auto *state = static_cast<EntropyState *>(context);
    ++state->calls;
    for (size_t i = 0; i < length; ++i)
        output[i] = state->allZero ? 0 : static_cast<uint8_t>(state->next + i);
    state->next = static_cast<uint8_t>(state->next + 13);
    return !state->fail;
}

static void fillKey(uint8_t *key, uint8_t seed) {
    for (size_t i = 0; i < AdminSessionPasskeys::kPublicKeyBytes; ++i)
        key[i] = static_cast<uint8_t>(seed + i);
}

static bool isZero(const uint8_t *bytes, size_t length) {
    uint8_t combined = 0;
    for (size_t i = 0; i < length; ++i)
        combined = static_cast<uint8_t>(combined | bytes[i]);
    return combined == 0;
}

static void testIssueStableRotationAndExactBoundaries() {
    AdminSessionPasskeys cache;
    EntropyState rng;
    uint8_t key[32], token[8]{}, stable[8]{}, rotated[8]{};
    fillKey(key, 4);
    assert(cache.issueAfterCallerAuthorization(1001, key, 100, fakeEntropy, &rng, token));
    assert(rng.calls == 1 && cache.activeCount() == 1);
    memcpy(stable, token, sizeof(stable));
    assert(cache.issueAfterCallerAuthorization(1001, key, 100 + 149999, fakeEntropy, &rng, token));
    assert(rng.calls == 1 && memcmp(token, stable, sizeof(token)) == 0);
    assert(cache.validateAfterCallerAuthorization(1001, key, 100 + 299999, stable));
    const size_t beforeValidation = cache.activeCount();
    assert(!cache.validateAfterCallerAuthorization(1001, key, 100 + 300000, stable));
    assert(cache.activeCount() == beforeValidation); // validation is non-mutating

    assert(cache.issueAfterCallerAuthorization(1001, key, 100 + 150000, fakeEntropy, &rng, token));
    assert(rng.calls == 2 && memcmp(token, stable, sizeof(token)) != 0);
    memcpy(rotated, token, sizeof(rotated));
    assert(!cache.validateAfterCallerAuthorization(1001, key, 100 + 150000, stable));
    assert(cache.validateAfterCallerAuthorization(1001, key, 100 + 150000 + 299999, rotated));
    assert(!cache.validateAfterCallerAuthorization(1001, key, 100 + 150000 + 300000, rotated));
}

static void testWraparoundAndSweep() {
    AdminSessionPasskeys cache;
    EntropyState rng;
    uint8_t key[32], token[8]{}, next[8]{};
    fillKey(key, 18);
    const uint32_t issued = UINT32_MAX - 25U;
    assert(cache.issueAfterCallerAuthorization(2002, key, issued, fakeEntropy, &rng, token));
    const uint32_t beforeRotation = static_cast<uint32_t>(issued + 149999U);
    assert(cache.issueAfterCallerAuthorization(2002, key, beforeRotation, fakeEntropy, &rng, next));
    assert(memcmp(token, next, sizeof(token)) == 0 && rng.calls == 1);
    const uint32_t rotateBoundary = static_cast<uint32_t>(issued + 150000U);
    assert(cache.issueAfterCallerAuthorization(2002, key, rotateBoundary, fakeEntropy, &rng, next));
    assert(rng.calls == 2 && memcmp(token, next, sizeof(token)) != 0);
    assert(cache.validateAfterCallerAuthorization(2002, key,
        static_cast<uint32_t>(rotateBoundary + 299999U), next));
    const size_t activeBeforeExpiredCheck = cache.activeCount();
    assert(!cache.validateAfterCallerAuthorization(2002, key,
        static_cast<uint32_t>(rotateBoundary + 300000U), next));
    assert(cache.activeCount() == activeBeforeExpiredCheck);
    assert(cache.sweep(static_cast<uint32_t>(rotateBoundary + 300000U)) == 1);
    assert(cache.activeCount() == 0 && cache.allSlotsWiped());
}

static void testBindingAndNonMutatingValidation() {
    AdminSessionPasskeys cache;
    EntropyState rng;
    uint8_t keyA[32], keyB[32], token[8]{}, wrong[8]{};
    fillKey(keyA, 3); fillKey(keyB, 9);
    assert(cache.issueAfterCallerAuthorization(3003, keyA, 1, fakeEntropy, &rng, token));
    memcpy(wrong, token, sizeof(wrong)); wrong[4] ^= 0x80;
    assert(!cache.validateAfterCallerAuthorization(3004, keyA, 2, token));
    assert(!cache.validateAfterCallerAuthorization(3003, keyB, 2, token));
    assert(!cache.validateAfterCallerAuthorization(3003, keyA, 2, wrong));
    assert(cache.validateAfterCallerAuthorization(3003, keyA, 2, token));
    assert(cache.activeCount() == 1);
    assert(cache.revokePeerKey(3003, keyB) == 0 && cache.activeCount() == 1);
    assert(cache.revokePeerKey(3003, keyA) == 1);
    assert(cache.activeCount() == 0 && cache.allSlotsWiped());
    assert(!cache.validateAfterCallerAuthorization(3003, keyA, 2, token));
}

static void testFullCacheDoesNotEvictAndRevocationWipes() {
    AdminSessionPasskeys cache;
    EntropyState rng;
    uint8_t keys[4][32], tokens[4][8]{};
    for (unsigned i = 0; i < 4; ++i) fillKey(keys[i], static_cast<uint8_t>(20 + i * 8));
    for (unsigned i = 0; i < 3; ++i)
        assert(cache.issueAfterCallerAuthorization(4000 + i, keys[i], 10, fakeEntropy, &rng, tokens[i]));
    const unsigned callsBeforeFull = rng.calls;
    assert(!cache.issueAfterCallerAuthorization(4003, keys[3], 10, fakeEntropy, &rng, tokens[3]));
    assert(isZero(tokens[3], sizeof(tokens[3])) && rng.calls == callsBeforeFull);
    for (unsigned i = 0; i < 3; ++i)
        assert(cache.validateAfterCallerAuthorization(4000 + i, keys[i], 11, tokens[i]));
    assert(cache.revokePeer(4001) == 1 && cache.activeCount() == 2);
    assert(!cache.validateAfterCallerAuthorization(4001, keys[1], 11, tokens[1]));
    assert(cache.revokeKey(keys[2]) == 1 && cache.activeCount() == 1);
    cache.clearAll();
    assert(cache.activeCount() == 0 && cache.allSlotsWiped());
}

static void testClearInvalidatesTokenAcrossAllowlistChange() {
    AdminSessionPasskeys cache;
    EntropyState rng;
    uint8_t key[32], oldToken[8]{}, newToken[8]{};
    fillKey(key, 31);
    assert(cache.issueAfterCallerAuthorization(6006, key, 100, fakeEntropy, &rng, oldToken));
    // Production calls clearAll before any allowlist write, including a write
    // whose persistence result is uncertain. Re-adding the same key cannot
    // resurrect the old passkey.
    cache.clearAll();
    assert(cache.issueAfterCallerAuthorization(6006, key, 101, fakeEntropy, &rng, newToken));
    assert(memcmp(oldToken, newToken, sizeof(oldToken)) != 0);
    assert(!cache.validateAfterCallerAuthorization(6006, key, 102, oldToken));
    assert(cache.validateAfterCallerAuthorization(6006, key, 102, newToken));
}

static void testEntropyFailureFailsClosedAndWipes() {
    AdminSessionPasskeys cache;
    EntropyState rng;
    uint8_t key[32], token[8], out[8];
    fillKey(key, 42);
    rng.fail = true;
    memset(out, 0xA5, sizeof(out));
    assert(!cache.issueAfterCallerAuthorization(5005, key, 1, fakeEntropy, &rng, out));
    assert(isZero(out, sizeof(out)) && cache.activeCount() == 0 && cache.allSlotsWiped());

    rng.fail = false;
    assert(cache.issueAfterCallerAuthorization(5005, key, 2, fakeEntropy, &rng, token));
    rng.fail = true;
    memset(out, 0xA5, sizeof(out));
    assert(!cache.issueAfterCallerAuthorization(5005, key, 2 + 150000U, fakeEntropy, &rng, out));
    assert(isZero(out, sizeof(out)) && cache.activeCount() == 0 && cache.allSlotsWiped());

    rng.fail = false; rng.allZero = true;
    assert(!cache.issueAfterCallerAuthorization(5005, key, 300000, fakeEntropy, &rng, out));
    assert(isZero(out, sizeof(out)) && cache.allSlotsWiped());
}

int main() {
    testIssueStableRotationAndExactBoundaries();
    testWraparoundAndSweep();
    testBindingAndNonMutatingValidation();
    testFullCacheDoesNotEvictAndRevocationWipes();
    testClearInvalidatesTokenAcrossAllowlistChange();
    testEntropyFailureFailsClosedAndWipes();
    puts("PASS_ADMIN_SESSION_PASSKEYS_PURE");
    return 0;
}
