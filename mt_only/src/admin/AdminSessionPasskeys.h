// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mt::admin {

// RAM-only peer-bound passkeys for a future authenticated private Admin SET
// path. Authorization is deliberately not represented by this class: callers
// must authenticate the PKI sender and check its current public-key allowlist
// before issueAfterCallerAuthorization(), then repeat both checks immediately
// before applying each mutation.
class AdminSessionPasskeys final {
public:
    static constexpr size_t kCapacity = 3;
    static constexpr size_t kPublicKeyBytes = 32;
    static constexpr size_t kTokenBytes = 8;
    static constexpr uint32_t kRotateAfterMs = 150000U;
    static constexpr uint32_t kValidForMs = 300000U;

    using EntropyProvider = bool (*)(void *context, uint8_t *output, size_t length);

    AdminSessionPasskeys() = default;
    ~AdminSessionPasskeys() { clearAll(); }
    AdminSessionPasskeys(const AdminSessionPasskeys &) = delete;
    AdminSessionPasskeys &operator=(const AdminSessionPasskeys &) = delete;
    AdminSessionPasskeys(AdminSessionPasskeys &&) = delete;
    AdminSessionPasskeys &operator=(AdminSessionPasskeys &&) = delete;

    // Call only after the caller has authenticated PKI and checked the current
    // Admin allowlist. Copies a token solely into callerOutput. If an existing
    // peer-bound token is younger than 150 seconds, it remains stable; at the
    // exact 150-second boundary it rotates. No active slot is evicted.
    bool issueAfterCallerAuthorization(uint32_t peer, const uint8_t *publicKey,
                                       uint32_t nowMs, EntropyProvider entropy,
                                       void *entropyContext,
                                       uint8_t *callerOutput) {
        if (!callerOutput)
            return false;
        wipe(callerOutput, kTokenBytes);
        if (!validIdentity(peer, publicKey) || !entropy)
            return false;

        // Expired slots are safe to reclaim during issuance. Validation itself
        // never mutates the cache; callers may also run sweep() explicitly.
        sweep(nowMs);

        Entry *slot = nullptr;
        for (Entry &entry : entries_) {
            if (!active(entry) || entry.peer != peer)
                continue;
            if (constantTimeEqual(entry.publicKey, publicKey, kPublicKeyBytes)) {
                slot = &entry;
                break;
            }
            // A peer changing keys revokes its old session before replacement.
            wipeEntry(entry);
            slot = &entry;
            break;
        }
        if (slot && active(*slot) &&
            elapsed(nowMs, slot->issuedAtMs) < kRotateAfterMs) {
            copy(callerOutput, slot->token, kTokenBytes);
            return true;
        }
        if (!slot) {
            for (Entry &entry : entries_) {
                if (!active(entry)) {
                    slot = &entry;
                    break;
                }
            }
        }
        if (!slot)
            return false;

        // A failed rotation invalidates the prior token too; returning stale
        // credentials after an entropy failure would be fail-open behavior.
        wipeEntry(*slot);
        uint8_t candidate[kTokenBytes]{};
        const bool generated = entropy(entropyContext, candidate, sizeof(candidate));
        if (!generated || !hasNonZero(candidate, sizeof(candidate))) {
            wipe(candidate, sizeof(candidate));
            return false;
        }
        slot->peer = peer;
        copy(slot->publicKey, publicKey, kPublicKeyBytes);
        copy(slot->token, candidate, kTokenBytes);
        slot->issuedAtMs = nowMs;
        copy(callerOutput, candidate, kTokenBytes);
        wipe(candidate, sizeof(candidate));
        return true;
    }

    // Caller must recheck authenticated PKI and current Admin allowlist before
    // each use. The cache only checks its peer/key binding and passkey window.
    // This method is observational: it does not rotate, sweep, or wipe entries.
    bool validateAfterCallerAuthorization(uint32_t peer, const uint8_t *publicKey,
                                          uint32_t nowMs, const uint8_t *token) const {
        if (!validIdentity(peer, publicKey) || !token)
            return false;
        for (const Entry &entry : entries_) {
            if (!active(entry) || entry.peer != peer ||
                !constantTimeEqual(entry.publicKey, publicKey, kPublicKeyBytes))
                continue;
            const bool withinWindow = elapsed(nowMs, entry.issuedAtMs) < kValidForMs;
            const bool tokenMatches = constantTimeEqual(entry.token, token, kTokenBytes);
            return withinWindow && tokenMatches;
        }
        return false;
    }

    size_t sweep(uint32_t nowMs) {
        size_t removed = 0;
        for (Entry &entry : entries_) {
            if (active(entry) && elapsed(nowMs, entry.issuedAtMs) >= kValidForMs) {
                wipeEntry(entry);
                ++removed;
            }
        }
        return removed;
    }

    size_t revokePeer(uint32_t peer) {
        size_t removed = 0;
        for (Entry &entry : entries_) {
            if (active(entry) && entry.peer == peer) {
                wipeEntry(entry);
                ++removed;
            }
        }
        return removed;
    }

    size_t revokePeerKey(uint32_t peer, const uint8_t *publicKey) {
        if (!publicKey)
            return 0;
        size_t removed = 0;
        for (Entry &entry : entries_) {
            if (active(entry) && entry.peer == peer &&
                constantTimeEqual(entry.publicKey, publicKey, kPublicKeyBytes)) {
                wipeEntry(entry);
                ++removed;
            }
        }
        return removed;
    }

    size_t revokeKey(const uint8_t *publicKey) {
        if (!publicKey)
            return 0;
        size_t removed = 0;
        for (Entry &entry : entries_) {
            if (active(entry) &&
                constantTimeEqual(entry.publicKey, publicKey, kPublicKeyBytes)) {
                wipeEntry(entry);
                ++removed;
            }
        }
        return removed;
    }

    void clearAll() {
        for (Entry &entry : entries_)
            wipeEntry(entry);
    }

    size_t activeCount() const {
        size_t count = 0;
        for (const Entry &entry : entries_)
            count += active(entry) ? 1U : 0U;
        return count;
    }

    // Safe diagnostic for tests/closeout: reveals only whether every slot has
    // been zeroed, never the peer key or token bytes.
    bool allSlotsWiped() const {
        for (const Entry &entry : entries_) {
            if (active(entry) || entry.peer || entry.issuedAtMs ||
                !allZero(entry.publicKey, sizeof(entry.publicKey)) ||
                !allZero(entry.token, sizeof(entry.token)))
                return false;
        }
        return true;
    }

private:
    struct Entry {
        uint32_t peer;
        uint8_t publicKey[kPublicKeyBytes];
        uint8_t token[kTokenBytes];
        uint32_t issuedAtMs;
    };

    Entry entries_[kCapacity]{};

    static bool active(const Entry &entry) {
        // Issued tokens are required to be nonzero, so their wiped state is
        // also the occupancy bit without a separate padded bool per slot.
        return hasNonZero(entry.token, sizeof(entry.token));
    }

    static uint32_t elapsed(uint32_t nowMs, uint32_t thenMs) {
        return static_cast<uint32_t>(nowMs - thenMs);
    }

    static bool validIdentity(uint32_t peer, const uint8_t *publicKey) {
        return peer != 0 && peer != UINT32_MAX && publicKey &&
               hasNonZero(publicKey, kPublicKeyBytes);
    }

    static void wipe(void *buffer, size_t length) {
        volatile uint8_t *bytes = static_cast<volatile uint8_t *>(buffer);
        while (length--)
            *bytes++ = 0;
    }

    static void wipeEntry(Entry &entry) {
        wipe(&entry, sizeof(entry));
    }

    static void copy(uint8_t *destination, const uint8_t *source, size_t length) {
        for (size_t i = 0; i < length; ++i)
            destination[i] = source[i];
    }

    static bool constantTimeEqual(const uint8_t *left, const uint8_t *right,
                                  size_t length) {
        uint8_t difference = 0;
        for (size_t i = 0; i < length; ++i)
            difference = static_cast<uint8_t>(difference | (left[i] ^ right[i]));
        return difference == 0;
    }

    static bool hasNonZero(const uint8_t *bytes, size_t length) {
        uint8_t combined = 0;
        for (size_t i = 0; i < length; ++i)
            combined = static_cast<uint8_t>(combined | bytes[i]);
        return combined != 0;
    }

    static bool allZero(const uint8_t *bytes, size_t length) {
        uint8_t combined = 0;
        for (size_t i = 0; i < length; ++i)
            combined = static_cast<uint8_t>(combined | bytes[i]);
        return combined == 0;
    }
};

} // namespace mt::admin
