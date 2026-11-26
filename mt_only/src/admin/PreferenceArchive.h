// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "SettingsJournal.h"

namespace mt::admin {

// Device-local, typed-settings archive. It has no filesystem, NVS enumeration,
// identity-secret, nonce, contact, Bluetooth-bond, or runtime-history access.
class PreferenceArchive {
public:
  static constexpr size_t MaxSections = 16;
  static constexpr size_t MaxSectionBytes = 1024;
  static constexpr size_t CompareBytes = 128;
  static constexpr size_t ManifestHeaderBytes = 52;
  static constexpr size_t ManifestEntryBytes = 7;
  static constexpr size_t ManifestBytes = 168;
  static constexpr size_t RecordHeaderBytes = 52;
  static constexpr size_t RecordMaxBytes = RecordHeaderBytes + MaxSectionBytes + 4;
  static constexpr size_t IntentBytes = 8 + ManifestBytes + 4;

  struct SnapshotDomain {
    uint32_t node = 0;
    uint8_t publicKey[32]{};
  };

  // capture() reads a stable serialized section through bounded offset reads.
  // At offset zero, totalLength is the full section length; at other offsets,
  // it must return the same totalLength and copy min(capacity,total-offset).
  // The owner holds its normal settings/service lock for each whole operation.
  struct Callbacks {
    void *context = nullptr;
    bool (*ready)(void *) = nullptr;
    bool (*quiescent)(void *) = nullptr;
    int (*capture)(void *, uint8_t sectionId, size_t offset, uint8_t *out,
                   size_t capacity, size_t &totalLength) = nullptr;
    int (*validate)(void *, uint8_t sectionId, const uint8_t *bytes,
                    size_t length) = nullptr;
    int (*apply)(void *, uint8_t sectionId, const uint8_t *bytes,
                 size_t length) = nullptr;
  };

  enum class IntentState : uint8_t { Absent, Pending, Completed, Corrupt };
  enum class ArchiveState : uint8_t { Absent, Available, Removed, Corrupt };

private:
  static constexpr uint8_t Version = 1;
  static constexpr uint8_t Active = 1;
  static constexpr uint8_t Tombstone = 2;
  static constexpr uint8_t IntentPending = 1;
  static constexpr uint8_t IntentCompleted = 2;
  static constexpr size_t ManifestCrcOffset = ManifestBytes - 4;
  static constexpr size_t RecordPayloadOffset = RecordHeaderBytes;
  static constexpr size_t IntentManifestOffset = 8;
  static constexpr size_t IntentCrcOffset = IntentBytes - 4;

  struct Wipe {
    void *data;
    size_t size;
    ~Wipe() {
      volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
      while (size--) *p++ = 0;
    }
  };
  struct Entry {
    uint8_t id = 0;
    uint16_t length = 0;
    uint32_t crc = 0;
  };
  struct Manifest {
    bool present = false;
    uint8_t raw[ManifestBytes]{};
    uint8_t state = 0;
    uint8_t bank = 0;
    uint8_t count = 0;
    uint32_t selectorGeneration = 0;
    uint32_t snapshotGeneration = 0;
    SnapshotDomain domain{};
    Entry entries[MaxSections]{};
  };
  struct Intent {
    IntentState state = IntentState::Absent;
    uint8_t raw[IntentBytes]{};
  };
  struct ArchiveView {
    ArchiveState state = ArchiveState::Absent;
    int selected = -1;
    Manifest manifests[2]{};
  };

  bool frozen_ = false;

  static uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
  }
  static uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static void put16(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value);
    p[1] = uint8_t(value >> 8);
  }
  static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
  }
  static uint32_t crc32(const uint8_t *p, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
      crc ^= p[i];
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
    }
    return ~crc;
  }
  static bool domainValid(const SnapshotDomain &domain) {
    if (!domain.node) return false;
    uint8_t bits = 0;
    for (uint8_t byte : domain.publicKey) bits |= byte;
    return bits != 0;
  }
  static bool sameDomain(const SnapshotDomain &a, const SnapshotDomain &b) {
    return a.node == b.node && memcmp(a.publicKey, b.publicKey, sizeof(a.publicKey)) == 0;
  }
  static bool callbacksReady(const Callbacks &cb, bool requireCapture,
                             bool requireApply) {
    return cb.ready && cb.quiescent && cb.ready(cb.context) &&
           cb.quiescent(cb.context) && (!requireCapture || (cb.capture && cb.validate)) &&
           (!requireApply || cb.apply);
  }
  static const char *manifestKey(unsigned slot) {
    return slot ? "mtba/m1" : "mtba/m0";
  }
  static const char *intentKey() { return "mtba/ri"; }
  static bool sectionKey(unsigned bank, uint8_t id, char out[9]) {
    if (bank > 1 || id >= MaxSections) return false;
    const char *prefix = bank ? "mtba/b" : "mtba/a";
    memcpy(out, prefix, 6);
    out[6] = char('0' + id / 10);
    out[7] = char('0' + id % 10);
    out[8] = '\0';
    return true;
  }
  static bool sortedIds(const uint8_t *ids, size_t count) {
    if (!ids || !count || count > MaxSections) return false;
    for (size_t i = 0; i < count; ++i)
      if (ids[i] >= MaxSections || (i && ids[i] <= ids[i - 1])) return false;
    return true;
  }
  static uint32_t nextGeneration(uint32_t generation) { return generation + 1u; }

  static void encodeManifest(const Manifest &m, uint8_t out[ManifestBytes]) {
    memset(out, 0, ManifestBytes);
    out[0] = 'M'; out[1] = 'T'; out[2] = 'B'; out[3] = 'M';
    out[4] = Version; out[5] = m.state; out[6] = m.bank; out[7] = m.count;
    put32(out + 8, m.selectorGeneration);
    put32(out + 12, m.snapshotGeneration);
    put32(out + 16, m.domain.node);
    memcpy(out + 20, m.domain.publicKey, 32);
    for (size_t i = 0; i < m.count; ++i) {
      uint8_t *entry = out + ManifestHeaderBytes + i * ManifestEntryBytes;
      entry[0] = m.entries[i].id;
      put16(entry + 1, m.entries[i].length);
      put32(entry + 3, m.entries[i].crc);
    }
    put32(out + ManifestCrcOffset, crc32(out, ManifestCrcOffset));
  }
  static bool decodeManifest(const uint8_t in[ManifestBytes], Manifest &m) {
    if (in[0] != 'M' || in[1] != 'T' || in[2] != 'B' || in[3] != 'M' ||
        in[4] != Version || (in[5] != Active && in[5] != Tombstone) ||
        in[6] > 1 || in[7] > MaxSections ||
        get32(in + ManifestCrcOffset) != crc32(in, ManifestCrcOffset)) return false;
    if ((in[5] == Active && !in[7]) || (in[5] == Tombstone && in[7])) return false;
    m.state = in[5]; m.bank = in[6]; m.count = in[7];
    m.selectorGeneration = get32(in + 8);
    m.snapshotGeneration = get32(in + 12);
    m.domain.node = get32(in + 16);
    memcpy(m.domain.publicKey, in + 20, 32);
    if (!domainValid(m.domain)) return false;
    uint8_t previous = 0;
    for (size_t i = 0; i < MaxSections; ++i) {
      const uint8_t *entry = in + ManifestHeaderBytes + i * ManifestEntryBytes;
      if (i >= m.count) {
        for (size_t j = 0; j < ManifestEntryBytes; ++j) if (entry[j]) return false;
        continue;
      }
      if (entry[0] >= MaxSections || (i && entry[0] <= previous) ||
          get16(entry + 1) > MaxSectionBytes) return false;
      m.entries[i] = {entry[0], get16(entry + 1), get32(entry + 3)};
      previous = entry[0];
    }
    if (m.raw != in) memcpy(m.raw, in, ManifestBytes);
    return true;
  }
  static void encodeRecordHeader(uint8_t *out, uint8_t id, uint32_t generation,
                                 const SnapshotDomain &domain, uint16_t length) {
    memset(out, 0, RecordHeaderBytes);
    out[0] = 'M'; out[1] = 'T'; out[2] = 'B'; out[3] = 'R';
    out[4] = Version; out[5] = id;
    put32(out + 8, generation); put32(out + 12, domain.node);
    memcpy(out + 16, domain.publicKey, 32); put16(out + 48, length);
  }
  static bool recordHeaderValid(const uint8_t *in, uint8_t id, uint32_t generation,
                                const SnapshotDomain &domain, uint16_t length) {
    uint8_t expected[RecordHeaderBytes];
    encodeRecordHeader(expected, id, generation, domain, length);
    const bool valid = memcmp(in, expected, sizeof(expected)) == 0;
    volatile uint8_t *p = expected;
    for (size_t i = 0; i < sizeof(expected); ++i) p[i] = 0;
    return valid;
  }
  static bool recordKey(unsigned bank, uint8_t id, char out[9]) {
    return sectionKey(bank, id, out);
  }

  int writeSmallExact(SettingsBackend &backend, const char *key,
                      const uint8_t *expected, size_t size) {
    uint8_t readback[IntentBytes]{};
    Wipe wipe{readback, sizeof(readback)};
    if (!expected || size > sizeof(readback)) return -EINVAL;
    int rc = backend.write(key, expected, size);
    if (rc) { frozen_ = true; return rc < 0 ? rc : -EIO; }
    rc = backend.read(key, readback, size);
    if (rc != int(size) || memcmp(readback, expected, size)) {
      frozen_ = true;
      return rc < 0 ? rc : -EIO;
    }
    return 0;
  }

  int readIntent(SettingsBackend &backend, Intent &intent) const {
    intent = {};
    const int n = backend.read(intentKey(), intent.raw, sizeof(intent.raw));
    if (n == -ENOENT) return 0;
    if (n < 0) {
      if (n == -E2BIG) intent.state = IntentState::Corrupt;
      else return n;
    } else if (n != int(IntentBytes)) {
      intent.state = IntentState::Corrupt;
    } else {
      const uint8_t *p = intent.raw;
      Manifest bound{};
      const bool valid = p[0] == 'M' && p[1] == 'T' && p[2] == 'B' && p[3] == 'I' &&
          p[4] == Version && (p[5] == IntentPending || p[5] == IntentCompleted) &&
          !p[6] && !p[7] && get32(p + IntentCrcOffset) == crc32(p, IntentCrcOffset) &&
          decodeManifest(p + IntentManifestOffset, bound) && bound.state == Active;
      intent.state = valid ? (p[5] == IntentPending ? IntentState::Pending : IntentState::Completed)
                           : IntentState::Corrupt;
    }
    return 0;
  }
  static void encodeIntent(uint8_t state, const uint8_t manifest[ManifestBytes],
                           uint8_t out[IntentBytes]) {
    memset(out, 0, IntentBytes);
    out[0] = 'M'; out[1] = 'T'; out[2] = 'B'; out[3] = 'I';
    out[4] = Version; out[5] = state;
    memcpy(out + IntentManifestOffset, manifest, ManifestBytes);
    put32(out + IntentCrcOffset, crc32(out, IntentCrcOffset));
  }

  int verifyRecord(SettingsBackend &backend, const Manifest &manifest,
                   const Entry &entry, uint8_t *record) const {
    char key[9]{};
    if (!recordKey(manifest.bank, entry.id, key)) return -EINVAL;
    const size_t size = RecordHeaderBytes + entry.length + 4;
    const int n = backend.read(key, record, RecordMaxBytes);
    if (n < 0) return n;
    if (size_t(n) != size ||
        !recordHeaderValid(record, entry.id, manifest.snapshotGeneration,
                           manifest.domain, entry.length) ||
        get32(record + RecordHeaderBytes + entry.length) != crc32(record, RecordHeaderBytes + entry.length) ||
        crc32(record + RecordPayloadOffset, entry.length) != entry.crc) return -EINVAL;
    return 0;
  }

  int readManifest(SettingsBackend &backend, unsigned slot, Manifest &manifest) const {
    manifest = {};
    const int n = backend.read(manifestKey(slot), manifest.raw, sizeof(manifest.raw));
    if (n == -ENOENT) return 0;
    if (n < 0) {
      if (n == -E2BIG) { manifest.present = true; return -EINVAL; }
      return n;
    }
    manifest.present = true;
    if (n != int(ManifestBytes) || !decodeManifest(manifest.raw, manifest)) {
      manifest.present = true;
      return -EINVAL;
    }
    manifest.present = true;
    return 0;
  }

  int readArchive(SettingsBackend &backend, const SnapshotDomain &domain,
                  ArchiveView &view, uint8_t *record) const {
    view = {};
    for (unsigned slot = 0; slot < 2; ++slot) {
      const int rc = readManifest(backend, slot, view.manifests[slot]);
      if (rc) { view.state = ArchiveState::Corrupt; return rc; }
    }
    const bool p0 = view.manifests[0].present, p1 = view.manifests[1].present;
    if (!p0 && !p1) { view.state = ArchiveState::Absent; return 0; }
    if ((p0 && !sameDomain(view.manifests[0].domain, domain)) ||
        (p1 && !sameDomain(view.manifests[1].domain, domain)) ||
        (p0 && p1 && !sameDomain(view.manifests[0].domain, view.manifests[1].domain))) {
      view.state = ArchiveState::Corrupt; return -EINVAL;
    }
    if (p0 && p1) {
      const uint32_t delta = view.manifests[1].selectorGeneration -
                             view.manifests[0].selectorGeneration;
      if ((!delta && memcmp(view.manifests[0].raw, view.manifests[1].raw, ManifestBytes)) ||
          delta == 0x80000000u) { view.state = ArchiveState::Corrupt; return -EINVAL; }
      view.selected = !delta ? 0 : (delta < 0x80000000u ? 1 : 0);
    } else view.selected = p1 ? 1 : 0;

    // Verify every present manifest, not just the newest selector. The writer
    // clones the current selector before reusing a bank, so both generations
    // continue to reference intact records throughout a staged backup.
    for (unsigned slot = 0; slot < 2; ++slot) {
      const auto &m = view.manifests[slot];
      if (!m.present || m.state == Tombstone) continue;
      for (size_t i = 0; i < m.count; ++i) {
        const int rc = verifyRecord(backend, m, m.entries[i], record);
        if (rc) { view.state = ArchiveState::Corrupt; return rc; }
      }
    }
    view.state = view.manifests[view.selected].state == Active ?
        ArchiveState::Available : ArchiveState::Removed;
    return 0;
  }

  int captureValidated(const Callbacks &cb, uint8_t id, uint8_t *out,
                       size_t &length, uint32_t &digest) const {
    length = 0;
    int rc = cb.capture(cb.context, id, 0, out, MaxSectionBytes, length);
    if (rc) return rc < 0 ? rc : -EIO;
    if (length > MaxSectionBytes) return -E2BIG;
    rc = cb.validate(cb.context, id, out, length);
    if (rc) return rc < 0 ? rc : -EINVAL;
    digest = crc32(out, length);
    return 0;
  }

  int compareCaptured(const Callbacks &cb, uint8_t id, const uint8_t *bytes,
                      size_t length, uint8_t *compare) const {
    for (size_t offset = 0; offset < length;) {
      const size_t amount = (length - offset) < CompareBytes ?
          length - offset : CompareBytes;
      size_t total = 0;
      const int rc = cb.capture(cb.context, id, offset, compare, amount, total);
      if (rc || total != length || memcmp(compare, bytes + offset, amount))
        return rc < 0 ? rc : -EIO;
      offset += amount;
    }
    return 0;
  }

  int writeSection(SettingsBackend &backend, const Callbacks &cb,
                   const SnapshotDomain &domain, uint8_t bank, uint8_t id,
                   uint32_t generation, uint16_t expectedLength,
                   uint32_t expectedCrc, uint8_t *record,
                   uint8_t *compare) {
    size_t length = 0;
    uint32_t digest = 0;
    int rc = captureValidated(cb, id, record + RecordPayloadOffset, length, digest);
    if (rc || length != expectedLength || digest != expectedCrc)
      return rc ? rc : -EAGAIN;
    encodeRecordHeader(record, id, generation, domain, uint16_t(length));
    const size_t recordSize = RecordHeaderBytes + length + 4;
    put32(record + RecordHeaderBytes + length, crc32(record, RecordHeaderBytes + length));
    char key[9]{};
    if (!recordKey(bank, id, key)) return -EINVAL;
    rc = backend.write(key, record, recordSize);
    if (rc) { frozen_ = true; return rc < 0 ? rc : -EIO; }
    rc = backend.read(key, record, RecordMaxBytes);
    if (rc != int(recordSize) ||
        !recordHeaderValid(record, id, generation, domain, uint16_t(length)) ||
        get32(record + RecordHeaderBytes + length) != crc32(record, RecordHeaderBytes + length) ||
        crc32(record + RecordPayloadOffset, length) != expectedCrc) {
      frozen_ = true;
      return rc < 0 ? rc : -EIO;
    }
    rc = compareCaptured(cb, id, record + RecordPayloadOffset, length, compare);
    if (rc) { frozen_ = true; return rc; }
    return 0;
  }

  int writeManifest(SettingsBackend &backend, unsigned slot, Manifest &manifest) {
    uint8_t raw[ManifestBytes]{};
    encodeManifest(manifest, raw);
    // Keep the raw manifest in the parsed copy so a readback can be compared
    // exactly and later bound into a restore intent.
    memcpy(manifest.raw, raw, ManifestBytes);
    const int rc = writeSmallExact(backend, manifestKey(slot), raw, sizeof(raw));
    if (rc) frozen_ = true;
    volatile uint8_t *p = raw;
    for (size_t i = 0; i < sizeof(raw); ++i) p[i] = 0;
    return rc;
  }

  int writeIntent(SettingsBackend &backend, uint8_t state,
                  const uint8_t manifest[ManifestBytes]) {
    uint8_t raw[IntentBytes]{};
    Wipe wipe{raw, sizeof(raw)};
    encodeIntent(state, manifest, raw);
    const int rc = writeSmallExact(backend, intentKey(), raw, sizeof(raw));
    if (rc) frozen_ = true;
    return rc;
  }

  int restoreInternal(SettingsBackend &backend, const SnapshotDomain &domain,
                      const Callbacks &cb, bool explicitResume,
                      const uint8_t *requiredIds, size_t requiredCount) {
    if (frozen_) return -EIO;
    if (!domainValid(domain) || !callbacksReady(cb, false, true) || !cb.validate ||
        !sortedIds(requiredIds, requiredCount))
      return -EACCES;
    uint8_t record[RecordMaxBytes]{};
    uint8_t compare[CompareBytes]{};
    Wipe wipeRecord{record, sizeof(record)};
    Wipe wipeCompare{compare, sizeof(compare)};
    ArchiveView view{};
    int rc = readArchive(backend, domain, view, record);
    if (rc) return rc;
    if (view.state != ArchiveState::Available || view.selected < 0) return -ENOENT;
    const Manifest &manifest = view.manifests[view.selected];
    if (manifest.count != requiredCount) return -EINVAL;
    for (size_t i = 0; i < requiredCount; ++i)
      if (manifest.entries[i].id != requiredIds[i]) return -EINVAL;

    // Validate every section before creating the durable intent or applying
    // the first target write.
    for (size_t i = 0; i < manifest.count; ++i) {
      rc = verifyRecord(backend, manifest, manifest.entries[i], record);
      if (rc) return rc;
      rc = cb.validate(cb.context, manifest.entries[i].id,
                       record + RecordPayloadOffset, manifest.entries[i].length);
      if (rc) return rc < 0 ? rc : -EINVAL;
    }

    Intent intent{};
    rc = readIntent(backend, intent);
    if (rc) return rc;
    if (intent.state == IntentState::Corrupt) return -EINVAL;
    if (intent.state == IntentState::Pending) {
      if (!explicitResume || memcmp(intent.raw + IntentManifestOffset,
                                    manifest.raw, ManifestBytes)) return -EBUSY;
    } else if (explicitResume) {
      return -ENOENT;
    } else {
      // A normal restore is an explicit new operation even when it selects the
      // same archive as a previously completed restore. It must apply the
      // archived settings again if live settings have since changed.
      rc = writeIntent(backend, IntentPending, manifest.raw);
      if (rc) return rc;
    }

    // Apply is required to be idempotent for exact typed-section replacement:
    // a rebooted explicit resume may replay sections committed before a prior
    // interruption, while the exact-manifest intent remains pending.
    for (size_t i = 0; i < manifest.count; ++i) {
      const Entry &entry = manifest.entries[i];
      rc = verifyRecord(backend, manifest, entry, record);
      if (rc) { frozen_ = true; return rc; }
      rc = cb.apply(cb.context, entry.id, record + RecordPayloadOffset, entry.length);
      if (rc) { frozen_ = true; return rc < 0 ? rc : -EIO; }
    }
    rc = writeIntent(backend, IntentCompleted, manifest.raw);
    if (rc) { frozen_ = true; return rc; }
    return 0;
  }

  bool mutationAllowed(SettingsBackend &backend, const SnapshotDomain &domain,
                       const Callbacks &cb, bool capture, bool apply,
                       int &error) const {
    if (frozen_) { error = -EIO; return false; }
    if (!domainValid(domain) || !callbacksReady(cb, capture, apply)) {
      error = -EACCES; return false;
    }
    Intent intent{};
    error = readIntent(backend, intent);
    if (error) return false;
    if (intent.state == IntentState::Pending || intent.state == IntentState::Corrupt) {
      error = -EBUSY; return false;
    }
    return true;
  }

public:
  bool frozen() const { return frozen_; }

  // Reports persistent restore state without resuming, applying settings, or
  // changing storage. A corrupt record is a state, not an automatic repair.
  int inspectRestoreIntent(SettingsBackend &backend, IntentState &state) const {
    Intent intent{};
    const int rc = readIntent(backend, intent);
    if (rc) return rc;
    state = intent.state;
    return 0;
  }

  int backup(SettingsBackend &backend, const SnapshotDomain &domain,
             const Callbacks &cb, const uint8_t *ids, size_t count) {
    int error = 0;
    if (!mutationAllowed(backend, domain, cb, true, false, error)) return error;
    if (!sortedIds(ids, count)) return -EINVAL;
    uint8_t record[RecordMaxBytes]{};
    uint8_t compare[CompareBytes]{};
    Wipe wipeRecord{record, sizeof(record)};
    Wipe wipeCompare{compare, sizeof(compare)};
    ArchiveView view{};
    int rc = readArchive(backend, domain, view, record);
    if (rc) return rc;

    Entry planned[MaxSections]{};
    for (size_t i = 0; i < count; ++i) {
      size_t length = 0; uint32_t digest = 0;
      rc = captureValidated(cb, ids[i], record + RecordPayloadOffset, length, digest);
      if (rc) return rc;
      planned[i] = {ids[i], uint16_t(length), digest};
    }

    uint8_t targetBank = 0;
    uint8_t finalSlot = 0;
    uint32_t selectorGeneration = 0;
    if (view.selected >= 0) {
      const Manifest &selected = view.manifests[view.selected];
      targetBank = uint8_t(1u - selected.bank);
      const unsigned cloneSlot = unsigned(1 - view.selected);
      // Keep both valid selectors pointed at the selected intact snapshot while
      // the inactive bank is rewritten. This exact clone is intentionally the
      // same generation and bytes; readArchive permits only identical ties.
      rc = writeSmallExact(backend, manifestKey(cloneSlot), selected.raw, ManifestBytes);
      if (rc) { frozen_ = true; return rc; }
      finalSlot = uint8_t(view.selected);
      selectorGeneration = nextGeneration(selected.selectorGeneration);
    } else {
      targetBank = 0; finalSlot = 0; selectorGeneration = 1;
    }
    const uint32_t snapshotGeneration = selectorGeneration;
    for (size_t i = 0; i < count; ++i) {
      rc = writeSection(backend, cb, domain, targetBank, ids[i], snapshotGeneration,
                        planned[i].length, planned[i].crc, record, compare);
      if (rc) return rc;
    }

    Manifest next{};
    next.present = true; next.state = Active; next.bank = targetBank;
    next.count = uint8_t(count); next.selectorGeneration = selectorGeneration;
    next.snapshotGeneration = snapshotGeneration; next.domain = domain;
    for (size_t i = 0; i < count; ++i) next.entries[i] = planned[i];
    rc = writeManifest(backend, finalSlot, next);
    if (rc) return rc;
    ArchiveView verified{};
    rc = readArchive(backend, domain, verified, record);
    if (rc || verified.state != ArchiveState::Available || verified.selected != finalSlot ||
        memcmp(verified.manifests[finalSlot].raw, next.raw, ManifestBytes)) {
      frozen_ = true;
      return rc < 0 ? rc : -EIO;
    }
    return 0;
  }

  // Logical removal only: commit a tombstone selector. No old bank is erased
  // or deleted, and no fallback to an older manifest occurs after selection.
  int remove(SettingsBackend &backend, const SnapshotDomain &domain,
             const Callbacks &cb) {
    int error = 0;
    if (!mutationAllowed(backend, domain, cb, false, false, error)) return error;
    uint8_t record[RecordMaxBytes]{};
    Wipe wipeRecord{record, sizeof(record)};
    ArchiveView view{};
    int rc = readArchive(backend, domain, view, record);
    if (rc) return rc;
    if (view.selected >= 0 && view.state == ArchiveState::Removed) return 0;
    Manifest tomb{};
    tomb.present = true; tomb.state = Tombstone; tomb.count = 0; tomb.domain = domain;
    unsigned slot = 0;
    if (view.selected >= 0) {
      const Manifest &selected = view.manifests[view.selected];
      tomb.bank = selected.bank;
      tomb.selectorGeneration = nextGeneration(selected.selectorGeneration);
      tomb.snapshotGeneration = selected.snapshotGeneration;
      slot = unsigned(1 - view.selected);
    } else {
      tomb.bank = 0; tomb.selectorGeneration = 1;
      tomb.snapshotGeneration = 1; slot = 0;
    }
    rc = writeManifest(backend, slot, tomb);
    if (rc) return rc;
    ArchiveView verified{};
    rc = readArchive(backend, domain, verified, record);
    if (rc || verified.state != ArchiveState::Removed || verified.selected != int(slot)) {
      frozen_ = true;
      return rc < 0 ? rc : -EIO;
    }
    return 0;
  }

  int restore(SettingsBackend &backend, const SnapshotDomain &domain,
              const Callbacks &cb, const uint8_t *requiredIds,
              size_t requiredCount) {
    return restoreInternal(backend, domain, cb, false, requiredIds, requiredCount);
  }
  // Explicit USB resume only. It requires a valid pending intent bound to the
  // exact selected manifest and current public identity domain.
  int resume(SettingsBackend &backend, const SnapshotDomain &domain,
            const Callbacks &cb, const uint8_t *requiredIds,
            size_t requiredCount) {
    return restoreInternal(backend, domain, cb, true, requiredIds, requiredCount);
  }
};

static_assert(PreferenceArchive::ManifestHeaderBytes +
                  PreferenceArchive::MaxSections * 7 + 4 ==
              PreferenceArchive::ManifestBytes,
              "archive manifest format size changed");
static_assert(PreferenceArchive::RecordMaxBytes == 1080,
              "archive record must remain one section plus fixed header");

} // namespace mt::admin
