#ifndef MT_PKI_PRIVATE_DUPLICATE_ACK_HPP
#define MT_PKI_PRIVATE_DUPLICATE_ACK_HPP

#include <cstddef>
#include <cstdint>
#include <string.h>

namespace mt {
namespace pki {

namespace detail {
constexpr std::size_t kMaxBody = 239;
}

// Two-entry per-peer RAM FIFO of repeat-ACK intents. No allocation, clock,
// TTL, digest, key/plaintext storage, radio/session/store calls, or crypto.
// remember() assumes the caller already authenticated and durably committed an
// armed original that requested ACK (wantACK) and is not a telemetry
// wantResponse; this helper cannot establish those preconditions itself.
// body is the authenticated PkiFrame.bytes ciphertext body, never plaintext.
// Each new entry carries one unclaimed repeat-ACK intent; on capacity only the
// oldest entry is evicted. This cache never changes durable replay IDs and does
// not provide full ACK parity: after eviction or reboot, duplicate packets
// remain suppressed.
class PrivateDuplicateAck {
public:
    bool remember(std::uint32_t id, const std::uint8_t *body, std::size_t n, std::uint8_t hop) {
        if (id == 0u || n < 1u || n > detail::kMaxBody || hop > 7u || body == nullptr)
            return false;
        for (std::size_t i = 0; i < 2; ++i)
            if (entries_[i].valid && entries_[i].id == id)
                return false;
        Entry &e = entries_[next_];
        e.id = id;
        e.len = n;
        e.hop = hop;
        e.claimed = false;
        e.valid = true;
        memcpy(e.body, body, n);
        next_ = (next_ + 1) % 2;
        return true;
    }

    bool claim(std::uint32_t id, const std::uint8_t *body, std::size_t n, std::uint8_t &outHop) {
        if (id == 0u || n < 1u || n > detail::kMaxBody || body == nullptr)
            return false;
        for (std::size_t i = 0; i < 2; ++i) {
            const Entry &e = entries_[i];
            if (e.valid && !e.claimed && e.id == id && e.len == n &&
                memcmp(e.body, body, n) == 0) {
                entries_[i].claimed = true;
                outHop = e.hop;
                return true;
            }
        }
        return false;
    }

    void clear() {
        memset(entries_, 0, sizeof(entries_));
        next_ = 0;
    }

private:
    struct Entry {
        std::uint32_t id;
        std::size_t len;
        std::uint8_t hop;
        bool claimed;
        bool valid;
        std::uint8_t body[detail::kMaxBody];
    };
    Entry entries_[2]{};
    std::size_t next_ = 0;
};

} // namespace pki
} // namespace mt
#endif
