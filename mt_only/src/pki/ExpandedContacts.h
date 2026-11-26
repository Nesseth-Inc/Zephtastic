// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiContactStore.h"
#include "PkiFreshRoot.h"

namespace mt::pki {
struct ExpandedContactFlash {
  virtual ~ExpandedContactFlash() = default;
  virtual int read(unsigned offset, void *out, size_t length) = 0;
  virtual int program(unsigned offset, const void *bytes, size_t length) = 0;
  virtual int eraseBank(unsigned bank) = 0;
  virtual void progress() {}
};

class RuntimePinDirectory {
public:
  static constexpr unsigned Limit = 198;

private:
  PkiPeerPin rows_[Limit]{};
  unsigned count_ = 0;

public:
  unsigned count() const { return count_; }
  const PkiPeerPin *at(size_t i) const { return i < count_ ? rows_ + i : nullptr; }
  int index(uint32_t node) const {
    for (unsigned i = 0; i < count_; ++i)
      if (rows_[i].node == node)
        return int(i);
    return -1;
  }
  const PkiPeerPin *find(uint32_t node) const {
    const int i = index(node);
    return i < 0 ? nullptr : rows_ + i;
  }
  void clear() {
    memset(rows_, 0, sizeof(rows_));
    count_ = 0;
  }
  void publish(unsigned i, const PkiPeerPin *pin) {
    if (pin) {
      rows_[i] = *pin;
      if (i == count_)
        ++count_;
    } else {
      --count_;
      memmove(rows_ + i, rows_ + i + 1, (count_ - i) * sizeof(*rows_));
      rows_[count_] = {};
    }
  }
  void import(const PkiPeerSet &legacy) {
    clear();
    for (unsigned i = 0; i < legacy.count(); ++i)
      publish(i, legacy.at(i));
  }
};

// The two exact 640-byte NVS names are the authority. A version-2 selector
// activates only its fully verified raw bank. Legacy readers reject the pair.
// The newest existing version-1 snapshot remains in place across the first
// conversion. Later selectors are strictly adjacent; neither IO errors nor a
// malformed newer slot can silently restore an older contact.
class ExpandedContacts {
public:
  static constexpr unsigned BankBytes = 8192, Banks = 4, Bytes = BankBytes * Banks;
  static constexpr unsigned Capacity = RuntimePinDirectory::Limit;
  enum class State { New, Legacy, Ready, Frozen };

private:
  static constexpr unsigned HeaderBytes = 64, RowBytes = 36, SelectorBytes = 640;
  static constexpr unsigned MirrorOffset = BankBytes - 12 - HeaderBytes;
  static_assert(HeaderBytes + Capacity * RowBytes <= MirrorOffset);
  static constexpr uint32_t Marker = 0x32435058u, Bank = 0x32425058u, Seal = 0x32535058u,
                            Committed = 0x32435041u;
  ExpandedContactFlash &flash_;
  RuntimePinDirectory rows_;
  IdentityBinding local_{};
  uint32_t root_ = 0, generation_ = 0;
  unsigned slot_ = 0;
  State state_ = State::New;
  struct Head {
    uint32_t gen, crc, count;
  };
  using Root = PkiFreshRoot;
  static uint32_t crcStep(uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) {
      crc ^= *p++;
      for (unsigned b = 0; b < 8; ++b)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
  }
  static bool ff(const uint8_t *p, size_t n) {
    while (n--)
      if (*p++ != 0xff)
        return false;
    return true;
  }
  static unsigned addr(uint32_t gen) { return (gen % Banks) * BankBytes; }
  int freeze(int rc) {
    state_ = State::Frozen;
    rows_.clear();
    return rc < 0 ? rc : -EIO;
  }
  void header(uint8_t *out, uint32_t magic, const Head &h) const {
    memset(out, 0, HeaderBytes);
    Root::put(out, magic);
    Root::put(out + 4, h.gen);
    Root::put(out + 8, local_.node);
    memcpy(out + 12, local_.publicKey, 32);
    Root::put(out + 44, local_.generation);
    Root::put(out + 48, root_);
    Root::put(out + 52, h.count);
    Root::put(out + 56, h.crc);
    Root::put(out + 60, Root::checksum(out, 60));
  }
  bool selector(const uint8_t *raw, Head &h) const {
    h = {Root::word(raw + 4), Root::word(raw + 56), Root::word(raw + 52)};
    uint8_t expected[HeaderBytes];
    header(expected, Marker, h);
    if (!h.gen || h.count > Capacity || memcmp(expected, raw, HeaderBytes) ||
        Root::word(raw + 636) != Root::checksum(raw, 636))
      return false;
    for (unsigned i = HeaderBytes; i < 636; ++i)
      if (raw[i])
        return false;
    return true;
  }
  static void encode(uint8_t *raw, const PkiPeerPin &pin) {
    Root::put(raw, pin.node);
    memcpy(raw + 4, pin.publicKey, 32);
  }
  static PkiPeerPin decode(const uint8_t *raw) {
    PkiPeerPin pin;
    pin.node = Root::word(raw);
    memcpy(pin.publicKey, raw + 4, 32);
    return pin;
  }
  struct Edit {
    unsigned index, count;
    const PkiPeerPin *pin;
    const PkiPeerPin &at(const RuntimePinDirectory &old, unsigned i) const {
      if (pin && i == index)
        return *pin;
      return *old.at(!pin && i >= index ? i + 1 : i);
    }
  };
  // Each erase sector has an owner header so a reset can resume after the
  // first sector of a two-sector bank was erased.
  int bankOwnership(unsigned bank, const IdentityBinding &local, uint32_t root) {
    const unsigned base = bank * BankBytes;
    uint8_t raw[HeaderBytes];
    bool owned = false;
    const unsigned offsets[2] = {0, MirrorOffset};
    for (unsigned offset : offsets) {
      int rc = flash_.read(base + offset, raw, sizeof(raw));
      if (rc)
        return rc;
      if (Root::word(raw) != Bank || Root::word(raw + 60) != Root::checksum(raw, 60))
        continue;
      if (!Root::word(raw + 4) || Root::word(raw + 4) % Banks != bank || Root::word(raw + 8) != local.node ||
          memcmp(raw + 12, local.publicKey, 32) || Root::word(raw + 44) != local.generation ||
          Root::word(raw + 48) != root || Root::word(raw + 52) > Capacity)
        return -EXDEV;
      owned = true;
    }
    if (owned)
      return 0;
    for (unsigned at = 0; at < BankBytes; at += sizeof(raw)) {
      const int rc = flash_.read(base + at, raw, sizeof(raw));
      if (rc)
        return rc;
      if (!ff(raw, sizeof(raw)))
        return -ENOTEMPTY;
      if (!(at % 1024))
        flash_.progress();
    }
    return 0;
  }
  int checkBank(const Head &h, bool publish, const Edit *intended = nullptr) {
    const unsigned base = addr(h.gen);
    uint8_t raw[HeaderBytes], expected[HeaderBytes];
    header(expected, Bank, h);
    int rc = flash_.read(base, raw, sizeof(raw));
    if (rc)
      return rc;
    if (memcmp(expected, raw, sizeof(raw)))
      return -EILSEQ;
    rc = flash_.read(base + MirrorOffset, raw, sizeof(raw));
    if (rc)
      return rc;
    if (memcmp(expected, raw, sizeof(raw)))
      return -EILSEQ;
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < h.count; ++i) {
      rc = flash_.read(base + HeaderBytes + i * RowBytes, raw, RowBytes);
      if (rc)
        return rc;
      crc = crcStep(crc, raw, RowBytes);
      if (intended) {
        encode(expected, intended->at(rows_, i));
        if (memcmp(expected, raw, RowBytes))
          return -EILSEQ;
      }
      if (publish) {
        const auto pin = decode(raw);
        PkiPeerSet valid;
        if (!valid.assignTrusted(local_, &pin, 1) || rows_.find(pin.node))
          return -EILSEQ;
        for (unsigned j = 0; j < i; ++j)
          if (!memcmp(rows_.at(j)->publicKey, pin.publicKey, 32))
            return -EILSEQ;
        rows_.publish(i, &pin);
      }
      if ((i & 7) == 7)
        flash_.progress();
    }
    if (~crc != h.crc)
      return -EILSEQ;
    rc = flash_.read(base + BankBytes - 8, raw, 8);
    if (rc)
      return rc;
    return Root::word(raw) == h.crc && Root::word(raw + 4) == Seal ? 0 : -EILSEQ;
  }
  int store(SettingsBackend &nvs, const Edit &edit, bool initial) {
    if (generation_ == UINT32_MAX)
      return -EOVERFLOW;
    Head h{generation_ + 1, 0, edit.count};
    uint8_t row[RowBytes];
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < h.count; ++i) {
      encode(row, edit.at(rows_, i));
      crc = crcStep(crc, row, sizeof(row));
    }
    h.crc = ~crc;
    const unsigned base = addr(h.gen);
    uint8_t head[HeaderBytes], seal[8];
    header(head, Bank, h);
    Root::put(seal, h.crc);
    Root::put(seal + 4, Seal);
    const int ownership = initial ? 0 : bankOwnership(h.gen % Banks, local_, root_);
    if (ownership && ownership != -ENOTEMPTY)
      return freeze(ownership);
    if (initial || ownership == -ENOTEMPTY) {
      // No erase during migration or when resuming a torn first header. Accept only blank bytes or an exact
      // prefix left by this same staged image. Foreign bytes anywhere hold.
      bool prefixEnded = false;
      const unsigned start = initial ? 0 : base, end = initial ? Bytes : base + BankBytes;
      for (unsigned at = start; at < end; at += 64) {
        uint8_t found[64];
        int rc = flash_.read(at, found, sizeof(found));
        if (rc)
          return rc;
        for (unsigned i = 0; i < sizeof(found); ++i) {
          const unsigned pos = at + i;
          uint8_t expected = 0xff;
          if (pos >= base && pos < base + BankBytes) {
            const unsigned offset = pos - base;
            if (offset < HeaderBytes)
              expected = head[offset];
            else if (offset < HeaderBytes + RowBytes * h.count) {
              const unsigned within = offset - HeaderBytes;
              if (within % RowBytes == 0)
                encode(row, edit.at(rows_, within / RowBytes));
              expected = row[within % RowBytes];
            } else if (offset >= MirrorOffset && offset < MirrorOffset + HeaderBytes)
              expected = head[offset - MirrorOffset];
            else if (offset >= BankBytes - 8)
              expected = seal[offset - (BankBytes - 8)];
          }
          if (expected != 0xff) {
            if (found[i] == 0xff)
              prefixEnded = true;
            else if (prefixEnded || found[i] != expected)
              return -ENOTEMPTY;
          } else if (found[i] != 0xff)
            return -ENOTEMPTY;
        }
      }
    } else {
      // A confirmed NVS selector plus the exact local binding owns this
      // dedicated extent. The active and previous bank are never overwritten.
      const int rc = flash_.eraseBank(h.gen % Banks);
      if (rc)
        return freeze(rc);
    }
    int rc = flash_.program(base, head, sizeof(head));
    for (unsigned i = 0; !rc && i < h.count; ++i) {
      encode(row, edit.at(rows_, i));
      rc = flash_.program(base + HeaderBytes + i * RowBytes, row, sizeof(row));
      if ((i & 15) == 15)
        flash_.progress();
    }
    if (!rc)
      rc = flash_.program(base + MirrorOffset, head, sizeof(head));
    if (!rc)
      rc = flash_.program(base + BankBytes - 8, seal, sizeof(seal));
    if (!rc)
      rc = checkBank(h, false, &edit);
    if (rc)
      return freeze(rc);
    uint8_t marker[SelectorBytes]{};
    header(marker, Marker, h);
    Root::put(marker + 636, Root::checksum(marker, 636));
    const unsigned next = 1 - slot_;
    const char *key = PkiContactStore::key(next);
    rc = nvs.write(key, marker, sizeof(marker));
    if (rc)
      return freeze(rc);
    uint8_t verify[SelectorBytes];
    rc = nvs.read(key, verify, sizeof(verify));
    if (rc != int(sizeof(verify)) || memcmp(verify, marker, sizeof(marker)))
      return freeze(rc < 0 ? rc : -EIO);
    uint8_t commit[4], final[4];
    Root::put(commit, Committed);
    rc = flash_.program(base + BankBytes - 12, commit, sizeof(commit));
    if (!rc)
      rc = flash_.read(base + BankBytes - 12, final, sizeof(final));
    if (rc || memcmp(commit, final, sizeof(commit)))
      return freeze(rc ? rc : -EIO);
    generation_ = h.gen;
    slot_ = next;
    state_ = State::Ready;
    return 0;
  }

public:
  explicit ExpandedContacts(ExpandedContactFlash &flash) : flash_(flash) {}
  State state() const { return state_; }
  bool ready() const { return state_ == State::Ready; }
  uint32_t generation() const { return ready() ? generation_ : 0; }
  const RuntimePinDirectory &pins() const { return rows_; }
  void progress() { flash_.progress(); }
  int load(SettingsBackend &nvs, const IdentityBinding &local, uint32_t root) {
    if (state_ != State::New)
      return -EACCES;
    local_ = local;
    root_ = root;
    uint8_t raw[2][SelectorBytes];
    Head heads[2]{};
    bool newer[2]{};
    for (unsigned s = 0; s < 2; ++s) {
      int rc = nvs.read(PkiContactStore::key(s), raw[s], SelectorBytes);
      if (rc != SelectorBytes)
        return freeze(rc < 0 ? rc : -EILSEQ);
      newer[s] = Root::word(raw[s]) == Marker;
      if (newer[s] && !selector(raw[s], heads[s]))
        return freeze(-EILSEQ);
    }
    // Reject a restored NVS head older than an acknowledged raw commit.
    // An interrupted new bank is unacknowledged and never confers membership.
    const uint32_t ceiling = heads[0].gen > heads[1].gen ? heads[0].gen : heads[1].gen;
    for (unsigned bank = 0; bank < Banks; ++bank) {
      uint8_t head[HeaderBytes], tail[4];
      int rc = flash_.read(bank * BankBytes, head, sizeof(head));
      if (!rc)
        rc = flash_.read((bank + 1) * BankBytes - 12, tail, sizeof(tail));
      if (rc)
        return freeze(rc);
      if (Root::word(tail) == Committed && Root::word(head) == Bank && Root::word(head + 8) == local_.node &&
          !memcmp(head + 12, local_.publicKey, 32) && Root::word(head + 48) == root_) {
        if (Root::word(head + 60) != Root::checksum(head, 60) || Root::word(head + 4) > ceiling)
          return freeze(-ESTALE);
      }
    }
    if (!newer[0] && !newer[1]) {
      state_ = State::Legacy;
      return 0;
    } // caller still validates both PCS1 slots
    if (newer[0] && newer[1]) {
      slot_ = heads[1].gen > heads[0].gen ? 1 : 0;
      if (heads[slot_].gen - heads[1 - slot_].gen != 1)
        return freeze(-EILSEQ);
      const int rc = checkBank(heads[1 - slot_], false);
      if (rc)
        return freeze(rc);
    } else {
      slot_ = newer[1] ? 1 : 0;
      PkiContactSnapshot::Value previous{};
      if (!PkiContactSnapshot::decode(raw[1 - slot_], SelectorBytes, local_, 1, root_, previous) ||
          previous.generation == UINT32_MAX || heads[slot_].gen != previous.generation + 1 ||
          heads[slot_].count != previous.count)
        return freeze(-EILSEQ);
      rows_.clear();
      int rc = checkBank(heads[slot_], true);
      if (rc)
        return freeze(rc);
      for (unsigned i = 0; i < previous.count; ++i)
        if (rows_.at(i)->node != previous.contacts[i].node ||
            memcmp(rows_.at(i)->publicKey, previous.contacts[i].publicKey, 32))
          return freeze(-EILSEQ);
    }
    if (newer[0] && newer[1]) {
      rows_.clear();
      const int rc = checkBank(heads[slot_], true);
      if (rc)
        return freeze(rc);
    }
    generation_ = heads[slot_].gen;
    state_ = State::Ready;
    return 0;
  }
  int enable(SettingsBackend &nvs, const PkiContactStore &old) {
    if (ready())
      return 0;
    PkiContactSnapshot::Value legacy;
    if (state_ != State::Legacy || !old.snapshot(legacy))
      return -EACCES;
    rows_.clear();
    for (unsigned i = 0; i < legacy.count; ++i)
      rows_.publish(i, legacy.contacts + i);
    generation_ = legacy.generation;
    slot_ = unsigned(old.activeSlot());
    const Edit same{legacy.count, legacy.count, nullptr};
    return store(nvs, same, true);
  }
  int put(SettingsBackend &nvs, const PkiPeerPin &pin, int (*before)(uint32_t)) {
    if (!ready())
      return -EACCES;
    PkiPeerSet valid;
    if (!valid.assignTrusted(local_, &pin, 1))
      return -EINVAL;
    const int old = rows_.index(pin.node);
    const unsigned index = old >= 0 ? unsigned(old) : rows_.count();
    for (unsigned j = 0; j < rows_.count(); ++j)
      if (j != index && !memcmp(rows_.at(j)->publicKey, pin.publicKey, 32))
        return -EEXIST;
    if (index == Capacity)
      return -ENOSPC;
    const bool same = old >= 0 && !memcmp(rows_.at(index)->publicKey, pin.publicKey, 32);
    if (!same && generation_ == UINT32_MAX)
      return -EOVERFLOW;
    if (before) {
      const int rc = before(pin.node);
      if (rc)
        return rc;
    }
    if (same)
      return 0;
    const Edit edit{index, rows_.count() + (old < 0 ? 1u : 0u), &pin};
    const int rc = store(nvs, edit, false);
    if (!rc)
      rows_.publish(index, &pin);
    return rc;
  }
  // Called only after the independent device-reset floor has been verified
  // and selected a successor identity. An unclaimed or foreign extent is
  // never erased. Repeated erases are safe before the old selectors retire.
  int retireDomain(SettingsBackend &nvs, const uint8_t retiring[32]) {
    if (!retiring)
      return -EINVAL;
    uint8_t raw[SelectorBytes];
    bool owned = false;
    IdentityBinding local{};
    uint32_t root = 0;
    for (unsigned i = 0; i < 2; ++i) {
      int n = nvs.read(PkiContactStore::key(i), raw, sizeof(raw));
      if (n == -ENOENT)
        continue;
      if (n != SelectorBytes)
        return n < 0 ? n : -EILSEQ;
      if (Root::word(raw) != Marker)
        continue;
      if (memcmp(raw + 12, retiring, 32) || Root::word(raw + 8) != bench::MtSignature::nodeId(retiring) ||
          Root::word(raw + 60) != Root::checksum(raw, 60) ||
          Root::word(raw + 636) != Root::checksum(raw, 636))
        return -EXDEV;
      if (owned && (local.generation != Root::word(raw + 44) || root != Root::word(raw + 48)))
        return -EXDEV;
      local.node = Root::word(raw + 8);
      local.generation = Root::word(raw + 44);
      memcpy(local.publicKey, retiring, 32);
      root = Root::word(raw + 48);
      owned = true;
    }
    if (!owned)
      return 0;
    for (unsigned i = 0; i < Banks; ++i) {
      const int rc = bankOwnership(i, local, root);
      if (rc)
        return rc;
    }
    for (unsigned i = 0; i < Banks; ++i) {
      bool dirty = false;
      for (unsigned at = 0; at < BankBytes; at += 64) {
        int rc = flash_.read(i * BankBytes + at, raw, 64);
        if (rc)
          return rc;
        if (!ff(raw, 64)) {
          dirty = true;
          break;
        }
      }
      if (dirty) {
        int rc = flash_.eraseBank(i);
        if (rc)
          return rc;
        for (unsigned at = 0; at < BankBytes; at += 64) {
          rc = flash_.read(i * BankBytes + at, raw, 64);
          if (rc || !ff(raw, 64))
            return rc ? rc : -EIO;
        }
      }
      flash_.progress();
    }
    return 0;
  }
  int remove(SettingsBackend &nvs, uint32_t node, const uint8_t *key) {
    const int i = rows_.index(node);
    if (!ready() || !key || i < 0 || memcmp(rows_.at(unsigned(i))->publicKey, key, 32))
      return -EACCES;
    const Edit edit{unsigned(i), rows_.count() - 1, nullptr};
    const int rc = store(nvs, edit, false);
    if (!rc)
      rows_.publish(unsigned(i), nullptr);
    return rc;
  }
};
} // namespace mt::pki
