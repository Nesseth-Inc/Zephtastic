// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mt::pki {

// Identity and immutable replay-domain anchor. The legacy* field names are
// wire-compatible; for the additive fresh-root format they carry its domain
// generation and CRC. The public key is not secret; no private key is stored.
struct PkiNonceCounterBinding {
  uint32_t node = 0;
  uint8_t publicKey[32]{};
  uint32_t legacyGeneration = 0;
  uint32_t legacyCrc = 0;
};

// Abstract two-slot durable store. read() returns exactly capacity bytes,
// -ENOENT for an absent slot, or another negative errno. write() returns zero
// only on success. A nonzero/uncertain write result permanently freezes this
// object; callers must reopen and burn whatever range may have committed.
class PkiNonceCounterStore {
public:
  virtual ~PkiNonceCounterStore() = default;
  virtual int read(unsigned slot, uint8_t *out, size_t capacity) = 0;
  virtual int write(unsigned slot, const uint8_t *data, size_t size) = 0;
};

// Identity-bound, dual-slot high-water allocator. initialize() writes only an
// identical genesis record to absent slots and never activates allocation.
// The integration supplies freshAuthority only after proving every domain
// record absent or exact genesis and committing the immutable root intent;
// activationProof is supplied only after validating the selected durable
// domain root (append BMIT or additive fresh-root selector). Those proofs are
// intentionally external to this component.
//
// Every reservation commits and reads back an inclusive high-water mark
// before issuing the first value in that block. Reopen burns the entire
// persisted block. There is no reset, repair, delete, or wrap path.
// CRC detects accidental damage; it is not an anti-rollback anchor. Callers
// must serialize all methods with the durable store and provide activation
// evidence that meets the product's rollback threat model.
class PkiNonceCounter {
public:
  static constexpr size_t RecordBytes = 64;
  static constexpr uint32_t DefaultBlockSize = 256;

  explicit PkiNonceCounter(PkiNonceCounterStore &store) : store_(store) {}

  PkiNonceCounter(const PkiNonceCounter &) = delete;
  PkiNonceCounter &operator=(const PkiNonceCounter &) = delete;

  // Read-only preflight for an explicit fresh-domain continuation. Existing
  // slots may be absent or the exact genesis pair for this identity/root;
  // non-genesis, malformed, or differently bound evidence is never replaced.
  int preflightGenesis(const PkiNonceCounterBinding &binding,bool freshAuthority) {
    if(state_==State::Active||state_==State::Frozen||state_==State::Exhausted)return -EACCES;
    if(!freshAuthority||!validBinding(binding))return -EACCES;
    uint8_t records[2][RecordBytes]{};ReadState reads[2]{};
    const int rc=readBoth(records,reads);if(rc)return rc;
    uint8_t genesis[RecordBytes]{};encode(genesis,binding,0,1);
    for(unsigned slot=0;slot<2;++slot){
      if(reads[slot]==ReadState::Error)return -EIO;
      if(reads[slot]==ReadState::Invalid)return -EILSEQ;
      if(reads[slot]==ReadState::Valid&&memcmp(records[slot],genesis,RecordBytes))return -EEXIST;
    }
    return 0;
  }

  // May create both identical genesis slots, or complete a partial genesis
  // where every present slot is exactly the matching genesis record. Never
// modifies a non-genesis, corrupt, or differently bound slot. Any uncertain
// bootstrap write/readback freezes this object; partial-genesis continuation
// requires a new object and fresh external authority. Does not enable
// reserve(); explicit activation is required through open().
  int initialize(const PkiNonceCounterBinding &binding,
                 bool freshAuthority) {
    if (state_ == State::Active || state_ == State::Frozen ||
        state_ == State::Exhausted)
      return -EACCES;
    if (!freshAuthority || !validBinding(binding))
      return -EACCES;

    uint8_t records[2][RecordBytes]{};
    ReadState reads[2]{};
    const int rc = readBoth(records, reads);
    if (rc) return rc;

    uint8_t genesis[RecordBytes]{};
    encode(genesis, binding, 0, 1);
    for (unsigned slot = 0; slot < 2; ++slot) {
      if (reads[slot] == ReadState::Invalid) return -EILSEQ;
      if (reads[slot] == ReadState::Valid) {
        if (memcmp(records[slot], genesis, RecordBytes) != 0) return -EACCES;
      }
    }

    for (unsigned slot = 0; slot < 2; ++slot) {
      if (reads[slot] != ReadState::Absent) continue;
      const int wr = store_.write(slot, genesis, sizeof(genesis));
      if (wr) {
        state_ = State::Frozen;
        return wr < 0 ? wr : -EIO;
      }
      uint8_t verify[RecordBytes]{};
      const int vr = store_.read(slot, verify, sizeof(verify));
      if (vr != int(sizeof(verify)) || memcmp(verify, genesis, sizeof(genesis))) {
        state_ = State::Frozen;
        return vr < 0 && vr != -ENOENT ? vr : -EIO;
      }
    }
    return 0;
  }

  // Requires both slots to be valid and mutually consistent. This performs no
  // writes. The persisted high-water block is burned in full after every open.
  int open(const PkiNonceCounterBinding &binding, bool activationProof) {
    close();
    if (!activationProof || !validBinding(binding))
      return -EACCES;

    uint8_t records[2][RecordBytes]{};
    ReadState reads[2]{};
    const int rc = readBoth(records, reads);
    if (rc) return rc;
    if (reads[0] != ReadState::Valid || reads[1] != ReadState::Valid)
      return -ENODATA;

    Decoded a{}, b{};
    if (!decode(records[0], a) || !decode(records[1], b)) return -EILSEQ;
    if (!sameBinding(a, binding) || !sameBinding(b, binding)) return -EXDEV;
    if (!consistent(a, b)) return -EILSEQ;

    binding_ = binding;
    if (a.recordGeneration > b.recordGeneration) {
      latest_ = a;
      latestSlot_ = 0;
    } else if (b.recordGeneration > a.recordGeneration) {
      latest_ = b;
      latestSlot_ = 1;
    } else {
      latest_ = a;
      // Identical generations are accepted only when the full canonical
      // records match. Slot 0 is the deterministic first write target.
      latestSlot_ = 0;
    }
    next_ = uint64_t(latest_.through) + 1u;
    blockEnd_ = latest_.through; // No pre-reboot reservation is reused.
    state_ = State::Active;
    return 0;
  }

  // out is always zero on refusal or uncertainty. A value becomes visible only
  // after the new high-water record has been written and read back exactly.
  int reserve(uint32_t &out) {
    out = 0;
    if (state_ != State::Active) return -EACCES;

    if (next_ > blockEnd_) {
      if (next_ > UINT32_MAX) {
        state_ = State::Exhausted;
        return -ENOSPC;
      }
      if (latest_.recordGeneration == UINT32_MAX) {
        state_ = State::Exhausted;
        return -EOVERFLOW;
      }

      const uint64_t start = next_;
      uint64_t end = start + uint64_t(DefaultBlockSize) - 1u;
      if (end > UINT32_MAX) end = UINT32_MAX;
      Decoded candidate = latest_;
      candidate.through = uint32_t(end);
      ++candidate.recordGeneration;
      uint8_t encoded[RecordBytes]{};
      encode(encoded, binding_, candidate.through,
             candidate.recordGeneration);

      const unsigned target = 1u - latestSlot_;
      const int wr = store_.write(target, encoded, sizeof(encoded));
      if (wr) {
        state_ = State::Frozen;
        return wr < 0 ? wr : -EIO;
      }
      uint8_t verify[RecordBytes]{};
      const int vr = store_.read(target, verify, sizeof(verify));
      Decoded checked{};
      if (vr != int(sizeof(verify)) || memcmp(verify, encoded, sizeof(encoded)) ||
          !decode(verify, checked) || !sameRecord(checked, candidate)) {
        state_ = State::Frozen;
        return vr < 0 && vr != -ENOENT ? vr : -EIO;
      }

      latest_ = candidate;
      latestSlot_ = target;
      blockEnd_ = end;
    }

    if (!next_ || next_ > blockEnd_ || next_ > UINT32_MAX) {
      state_ = State::Exhausted;
      return -ENOSPC;
    }
    out = uint32_t(next_++);
    return 0;
  }

  bool active() const { return state_ == State::Active; }
  uint32_t reservedThrough() const {
    return state_ == State::Active ? latest_.through : 0;
  }

  // Clears only volatile allocator state. Durable records and the identity
  // binding remain untouched.
  void close() {
    binding_ = {};
    latest_ = {};
    next_ = 0;
    blockEnd_ = 0;
    latestSlot_ = 0;
    state_ = State::Inactive;
  }

private:
  enum class State : uint8_t { Inactive, Active, Frozen, Exhausted };
  enum class ReadState : uint8_t { Absent, Valid, Invalid, Error };
  struct Decoded {
    PkiNonceCounterBinding binding{};
    uint32_t through = 0;
    uint32_t recordGeneration = 0;
  };

  PkiNonceCounterStore &store_;
  PkiNonceCounterBinding binding_{};
  Decoded latest_{};
  uint64_t next_ = 0;
  uint64_t blockEnd_ = 0;
  unsigned latestSlot_ = 0;
  State state_ = State::Inactive;

  static uint32_t crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
      crc ^= bytes[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
  }

  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
  }

  static bool validBinding(const PkiNonceCounterBinding &binding) {
    if (binding.node <= 1 || binding.node >= 0xfffffffeu ||
        !binding.legacyGeneration)
      return false;
    uint8_t nonzero = 0;
    for (uint8_t byte : binding.publicKey) nonzero |= byte;
    return nonzero != 0;
  }

  static uint64_t expectedThrough(uint32_t generation) {
    const uint64_t blocks = uint64_t(generation - 1u);
    const uint64_t through = blocks * uint64_t(DefaultBlockSize);
    return through > UINT32_MAX ? UINT32_MAX : through;
  }

  static uint32_t finalRecordGeneration() {
    return uint32_t((uint64_t(UINT32_MAX) + DefaultBlockSize - 1u) /
                    DefaultBlockSize + 1u);
  }

  static bool sameBinding(const Decoded &record,
                          const PkiNonceCounterBinding &binding) {
    return record.binding.node == binding.node &&
           record.binding.legacyGeneration == binding.legacyGeneration &&
           record.binding.legacyCrc == binding.legacyCrc &&
           memcmp(record.binding.publicKey, binding.publicKey, 32) == 0;
  }

  static bool sameRecord(const Decoded &a, const Decoded &b) {
    return a.binding.node == b.binding.node &&
           a.binding.legacyGeneration == b.binding.legacyGeneration &&
           a.binding.legacyCrc == b.binding.legacyCrc &&
           a.through == b.through &&
           a.recordGeneration == b.recordGeneration &&
           memcmp(a.binding.publicKey, b.binding.publicKey, 32) == 0;
  }

  static void encode(uint8_t out[RecordBytes],
                     const PkiNonceCounterBinding &binding,
                     uint32_t through, uint32_t recordGeneration) {
    memset(out, 0, RecordBytes);
    out[0] = 'Z'; out[1] = 'T'; out[2] = 'N'; out[3] = 'C';
    out[4] = 1; // version
    put32(out + 8, binding.node);
    memcpy(out + 12, binding.publicKey, 32);
    put32(out + 44, binding.legacyGeneration);
    put32(out + 48, binding.legacyCrc);
    put32(out + 52, through);
    put32(out + 56, recordGeneration);
    put32(out + 60, crc32(out, 60));
  }

  static bool decode(const uint8_t in[RecordBytes], Decoded &out) {
    if (in[0] != 'Z' || in[1] != 'T' || in[2] != 'N' || in[3] != 'C' ||
        in[4] != 1 || in[5] || in[6] || in[7] ||
        get32(in + 60) != crc32(in, 60))
      return false;
    Decoded candidate{};
    candidate.binding.node = get32(in + 8);
    memcpy(candidate.binding.publicKey, in + 12, 32);
    candidate.binding.legacyGeneration = get32(in + 44);
    candidate.binding.legacyCrc = get32(in + 48);
    candidate.through = get32(in + 52);
    candidate.recordGeneration = get32(in + 56);
    if (!validBinding(candidate.binding) || !candidate.recordGeneration ||
        candidate.recordGeneration > finalRecordGeneration() ||
        uint64_t(candidate.through) != expectedThrough(candidate.recordGeneration))
      return false;
    out = candidate;
    return true;
  }

  uint64_t nextThrough(uint32_t previous) const {
    const uint64_t start = uint64_t(previous) + 1u;
    uint64_t end = start + uint64_t(DefaultBlockSize) - 1u;
    if (end > UINT32_MAX) end = UINT32_MAX;
    return end;
  }

  bool consistent(const Decoded &a, const Decoded &b) const {
    if (a.binding.node != b.binding.node ||
        a.binding.legacyGeneration != b.binding.legacyGeneration ||
        a.binding.legacyCrc != b.binding.legacyCrc ||
        memcmp(a.binding.publicKey, b.binding.publicKey, 32) != 0)
      return false;
    if (a.recordGeneration == b.recordGeneration)
      return sameRecord(a, b);
    const Decoded &older = a.recordGeneration < b.recordGeneration ? a : b;
    const Decoded &newer = a.recordGeneration < b.recordGeneration ? b : a;
    if (older.recordGeneration == UINT32_MAX ||
        newer.recordGeneration != older.recordGeneration + 1u ||
        older.through == UINT32_MAX)
      return false;
    return uint64_t(newer.through) == nextThrough(older.through);
  }

  int readBoth(uint8_t records[2][RecordBytes], ReadState reads[2]) {
    for (unsigned slot = 0; slot < 2; ++slot) {
      const int rc = store_.read(slot, records[slot], RecordBytes);
      if (rc == -ENOENT) {
        reads[slot] = ReadState::Absent;
      } else if (rc < 0) {
        reads[slot] = ReadState::Error;
        return rc;
      } else if (rc != int(RecordBytes)) {
        reads[slot] = ReadState::Invalid;
      } else {
        Decoded decoded{};
        reads[slot] = decode(records[slot], decoded)
                          ? ReadState::Valid : ReadState::Invalid;
      }
    }
    return 0;
  }
};

static_assert(PkiNonceCounter::RecordBytes == 64,
              "nonce counter record wire size");
static_assert(sizeof(PkiNonceCounter) <= 128,
              "nonce counter must stay within its bounded RAM budget");

} // namespace mt::pki
