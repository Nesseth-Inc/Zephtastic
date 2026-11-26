// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "PkiContactSnapshot.h"
#include "../SettingsJournal.h"

namespace mt::pki {

// Inert two-slot prototype for the bounded active-contact snapshot.
// It does not activate PKI, touch the legacy journal/nonce allocator, or add
// replay state. Two 640-byte payload slots total 1,280 bytes before backend
// metadata/GC; this is not a storage or target-fit claim. The owner must
// serialize calls and explicitly initialize only after proving both keys absent
// and the immutable bindings are current.
class PkiContactStore {
public:
  static constexpr size_t RecordBytes = PkiContactSnapshot::RecordBytes;
  enum class State : uint8_t { Unloaded, Empty, Ready, Frozen };

  PkiContactStore() = default;
  PkiContactStore(const PkiContactStore &) = delete;
  PkiContactStore &operator=(const PkiContactStore &) = delete;

  State state() const { return state_; }
  bool ready() const { return state_ == State::Ready; }
  int activeSlot() const { return active_; }
  uint32_t generation() const { return ready() ? current_.generation : 0; }

  // Fixed names are disjoint from the existing mtidentity/state*,
  // mtpki/ledger*, and mtpki/nonces* records.
  static const char *key(unsigned slot) {
    return slot == 0 ? "mtpki/contacts0" :
           slot == 1 ? "mtpki/contacts1" : nullptr;
  }

  // Read-only exactly once. Both absent slots produce Empty and do not write.
  // A valid loaded store has two complete adjacent generations. A single
  // slot is partial genesis/commit evidence and fails closed without repair.
  int load(SettingsBackend &backend, const IdentityBinding &local,
           uint32_t legacyRootGeneration, uint32_t legacyRootCrc) {
    if (state_ != State::Unloaded) return -EACCES;
    if (!validBinding(local, legacyRootGeneration)) return -EINVAL;

    uint8_t raw[RecordBytes]{};
    PkiContactSnapshot::Value second{};
    bool present[2]{};
    int n = backend.read(key(0), raw, sizeof(raw));
    if (n != -ENOENT) {
      if (n < 0 && n != -E2BIG) return freeze(n);
      present[0] = true;
      if (n != int(RecordBytes) ||
          !PkiContactSnapshot::decode(raw, RecordBytes, local,
                                      legacyRootGeneration, legacyRootCrc,
                                      current_)) {
        return freeze(-EILSEQ);
      }
    }

    memset(raw, 0, sizeof(raw));
    n = backend.read(key(1), raw, sizeof(raw));
    if (n != -ENOENT) {
      if (n < 0 && n != -E2BIG) return freeze(n);
      present[1] = true;
      if (n != int(RecordBytes) ||
          !PkiContactSnapshot::decode(raw, RecordBytes, local,
                                      legacyRootGeneration, legacyRootCrc,
                                      second)) {
        return freeze(-EILSEQ);
      }
    }

    local_ = local;
    rootGeneration_ = legacyRootGeneration;
    rootCrc_ = legacyRootCrc;
    if (!present[0] && !present[1]) {
      current_ = PkiContactSnapshot::Value{};
      active_ = -1;
      state_ = State::Empty;
      return 0;
    }
    if (!present[0] || !present[1]) return freeze(-EILSEQ);

    const uint32_t a = current_.generation;
    const uint32_t b = second.generation;
    if (a != UINT32_MAX && a + 1 == b) {
      current_ = second;
      active_ = 1;
    } else if (b != UINT32_MAX && b + 1 == a) {
      active_ = 0;
    } else {
      return freeze(-EILSEQ);
    }
    state_ = State::Ready;
    return 0;
  }

  // Explicit genesis only from an Empty load. Recheck both keys are still
  // absent, then write/readback adjacent generations 1 and 2 before publishing.
  // Invalid inputs do not write; any attempted-write uncertainty freezes.
  int initialize(SettingsBackend &backend, const PkiPeerPin *contacts,
                 size_t count) {
    if (state_ != State::Empty) return -EACCES;
    uint8_t record[RecordBytes]{};
    uint8_t readback[RecordBytes]{};
    if (!encodeRecord(local_, 1, rootGeneration_, rootCrc_, contacts, count,
                      record)) {
      return -EINVAL;
    }
    for (unsigned slot = 0; slot < 2; ++slot) {
      const int n = backend.read(key(slot), readback, sizeof(readback));
      if (n == -ENOENT) continue;
      if (n < 0 && n != -E2BIG) return freeze(n);
      return freeze(n == -E2BIG ? -EILSEQ : -EEXIST);
    }
    int wr = backend.write(key(0), record, sizeof(record));
    if (wr != 0) return freeze(wr < 0 ? wr : -EIO);
    int n = backend.read(key(0), readback, sizeof(readback));
    PkiContactSnapshot::Value first{};
    if (!verifyRecord(n, record, readback, local_, rootGeneration_, rootCrc_,
                      1, first)) {
      return freeze(n < 0 && n != -ENOENT ? n : -EIO);
    }

    if (!encodeRecord(local_, 2, rootGeneration_, rootCrc_, contacts, count,
                      record)) {
      return freeze(-EINVAL);
    }
    wr = backend.write(key(1), record, sizeof(record));
    if (wr != 0) return freeze(wr < 0 ? wr : -EIO);
    n = backend.read(key(1), readback, sizeof(readback));
    PkiContactSnapshot::Value verified{};
    if (!verifyRecord(n, record, readback, local_, rootGeneration_, rootCrc_,
                      2, verified)) {
      return freeze(n < 0 && n != -ENOENT ? n : -EIO);
    }
    // A normal load always requires the adjacent pair. The incomplete first
    // slot alone is deliberately not mountable after an interrupted genesis.
    current_ = verified;
    active_ = 1;
    state_ = State::Ready;
    return 0;
  }

  // Explicit one-time completion of an interrupted generation-1 genesis.
  // The caller must provide the independently replayed effective legacy
  // contacts and current identity/root binding. This is never an Empty-store
  // initializer: slot zero must already equal the exact canonical generation-1
  // record and slot one must be positively absent. Ready stores cannot resume.
  int resumeGenesis(SettingsBackend &backend,const IdentityBinding &local,
                    uint32_t legacyRootGeneration,uint32_t legacyRootCrc,
                    const PkiPeerPin *contacts,size_t count) {
    if(state_!=State::Unloaded&&state_!=State::Frozen)return -EACCES;
    if(!validBinding(local,legacyRootGeneration))return -EINVAL;
    uint8_t firstRecord[RecordBytes]{},secondRecord[RecordBytes]{},readback[RecordBytes]{};
    if(!encodeRecord(local,1,legacyRootGeneration,legacyRootCrc,contacts,count,firstRecord)||
       !encodeRecord(local,2,legacyRootGeneration,legacyRootCrc,contacts,count,secondRecord))return -EINVAL;

    // Recheck the precise recoverable state immediately before the one allowed
    // write. Any present/malformed slot1 or any noncanonical slot0 refuses.
    int n=backend.read(key(0),readback,sizeof(readback));
    if(n!=int(sizeof(readback)))return freeze(n==-ENOENT?-ENODATA:(n<0?n:-EILSEQ));
    PkiContactSnapshot::Value first{};
    if(!verifyRecord(n,firstRecord,readback,local,legacyRootGeneration,
                     legacyRootCrc,1,first))return freeze(-EILSEQ);
    n=backend.read(key(1),readback,sizeof(readback));
    if(n!=-ENOENT)return freeze(n<0?n:(n==int(sizeof(readback))?-EEXIST:-EILSEQ));

    const int wr=backend.write(key(1),secondRecord,sizeof(secondRecord));
    if(wr!=0)return freeze(wr<0?wr:-EIO);
    n=backend.read(key(1),readback,sizeof(readback));
    PkiContactSnapshot::Value second{};
    if(!verifyRecord(n,secondRecord,readback,local,legacyRootGeneration,
                     legacyRootCrc,2,second))return freeze(n<0&&n!=-ENOENT?n:-EIO);

    // Final adjacent-pair readback: do not publish after only a slot1 check.
    n=backend.read(key(0),readback,sizeof(readback));
    PkiContactSnapshot::Value verifiedFirst{};
    if(!verifyRecord(n,firstRecord,readback,local,legacyRootGeneration,
                     legacyRootCrc,1,verifiedFirst))return freeze(n<0&&n!=-ENOENT?n:-EIO);
    n=backend.read(key(1),readback,sizeof(readback));
    PkiContactSnapshot::Value verifiedSecond{};
    if(!verifyRecord(n,secondRecord,readback,local,legacyRootGeneration,
                     legacyRootCrc,2,verifiedSecond))return freeze(n<0&&n!=-ENOENT?n:-EIO);
    if(!sameContacts(verifiedFirst,verifiedSecond))return freeze(-EILSEQ);

    local_=local;rootGeneration_=legacyRootGeneration;rootCrc_=legacyRootCrc;
    current_=verifiedSecond;active_=1;state_=State::Ready;return 0;
  }

  // Commit a complete replacement to the inactive slot. RAM state is published
  // only after exact readback and a second semantic decode. There is no wrap,
  // implicit initialization, replay-domain, or partial update behavior.
  int replace(SettingsBackend &backend, const PkiPeerPin *contacts,
              size_t count) {
    if (!ready()) return -EACCES;
    if (current_.generation == UINT32_MAX) return -EOVERFLOW;
    const uint32_t next = current_.generation + 1;
    uint8_t record[RecordBytes]{};
    if (!encodeRecord(local_, next, rootGeneration_, rootCrc_, contacts, count,
                      record)) {
      return -EINVAL;
    }
    const unsigned slot = active_ == 0 ? 1 : 0;
    const int wr = backend.write(key(slot), record, sizeof(record));
    if (wr != 0) return freeze(wr < 0 ? wr : -EIO);

    uint8_t readback[RecordBytes]{};
    const int n = backend.read(key(slot), readback, sizeof(readback));
    if (n != int(sizeof(record)) || memcmp(record, readback, sizeof(record)))
      return freeze(n < 0 && n != -ENOENT ? n : -EIO);

    PkiContactSnapshot::Value verified{};
    if (!PkiContactSnapshot::decode(readback, sizeof(readback), local_,
                                    rootGeneration_, rootCrc_, verified) ||
        verified.generation != next) {
      return freeze(-EILSEQ);
    }
    current_ = verified;
    active_ = static_cast<int8_t>(slot);
    return 0;
  }

  bool snapshot(PkiContactSnapshot::Value &out) const {
    if (!ready()) return false;
    out = current_;
    return true;
  }

private:
  static bool encodeRecord(const IdentityBinding &local,uint32_t generation,
                           uint32_t rootGeneration,uint32_t rootCrc,
                           const PkiPeerPin *contacts,size_t count,
                           uint8_t record[RecordBytes]) {
    return PkiContactSnapshot::encode(local,generation,rootGeneration,rootCrc,
                                      contacts,count,record,RecordBytes);
  }

  static bool verifyRecord(int actualSize,const uint8_t expected[RecordBytes],
                           const uint8_t actual[RecordBytes],
                           const IdentityBinding &local,uint32_t rootGeneration,
                           uint32_t rootCrc,uint32_t generation,
                           PkiContactSnapshot::Value &decoded) {
    return actualSize==int(RecordBytes)&&!memcmp(expected,actual,RecordBytes)&&
      PkiContactSnapshot::decode(actual,RecordBytes,local,rootGeneration,rootCrc,decoded)&&
      decoded.generation==generation;
  }

  static bool sameContacts(const PkiContactSnapshot::Value &a,
                           const PkiContactSnapshot::Value &b) {
    if(a.count!=b.count)return false;
    for(size_t i=0;i<a.count;++i)
      if(a.contacts[i].node!=b.contacts[i].node||
         memcmp(a.contacts[i].publicKey,b.contacts[i].publicKey,32))return false;
    return true;
  }

  static bool validBinding(const IdentityBinding &local, uint32_t rootGeneration) {
    if (rootGeneration == 0) return false;
    PkiPeerSet check;
    return check.assignEmpty(local);
  }

  int freeze(int error) {
    current_ = PkiContactSnapshot::Value{};
    active_ = -1;
    state_ = State::Frozen;
    return error < 0 ? error : -EIO;
  }

  IdentityBinding local_{};
  uint32_t rootGeneration_ = 0;
  uint32_t rootCrc_ = 0;
  PkiContactSnapshot::Value current_{};
  int8_t active_ = -1;
  State state_ = State::Unloaded;
};

} // namespace mt::pki
