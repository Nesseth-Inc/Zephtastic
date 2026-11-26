// SPDX-License-Identifier: GPL-3.0-only
// Synthetic public Admin keys only; no real identity, private key, or device.
#include "admin/AdminKeyJournal.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
using mt::SettingsBackend;
using mt::admin::AdminKeyJournal;
using mt::admin::AdminKeySettings;
using Bytes = std::vector<uint8_t>;

struct MemoryBackend final : SettingsBackend {
  std::map<std::string, Bytes> rows;
  unsigned reads = 0;
  unsigned writes = 0;
  int readError = 0;
  std::string readErrorKey;
  int writeErrorBefore = 0;
  int writeErrorAfter = 0;
  bool corruptReadback = false;
  bool shortReadback = false;
  std::string lastWrite;

  int read(const char *key, uint8_t *out, size_t capacity) override {
    ++reads;
    if (readError && (readErrorKey.empty() || readErrorKey == key)) return readError;
    const auto found = rows.find(key);
    if (found == rows.end()) return -ENOENT;
    if (found->second.size() > capacity) return -E2BIG;
    const size_t length = found->second.size();
    if (length) memcpy(out, found->second.data(), length);
    if (corruptReadback && key == lastWrite && length)
      out[length / 2] ^= 0x80;
    if (shortReadback && key == lastWrite && length)
      return static_cast<int>(length - 1);
    return static_cast<int>(length);
  }

  int write(const char *key, const uint8_t *data, size_t length) override {
    ++writes;
    lastWrite = key;
    if (writeErrorBefore) return writeErrorBefore;
    rows[key] = Bytes(data, data + length);
    return writeErrorAfter;
  }
};

AdminKeySettings makeSettings(std::initializer_list<uint8_t> markers) {
  std::array<uint8_t, AdminKeySettings::PackedBytes> packed{};
  size_t offset = 0;
  for (uint8_t marker : markers) {
    memset(packed.data() + offset, marker, AdminKeySettings::KeyBytes);
    offset += AdminKeySettings::KeyBytes;
  }
  AdminKeySettings settings;
  assert(settings.assign(markers.size(), offset ? packed.data() : nullptr, offset));
  return settings;
}

uint32_t crc32(const uint8_t *bytes, size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
  }
  return ~crc;
}

void put32(uint8_t *bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
}

Bytes record(uint32_t generation, const AdminKeySettings &settings) {
  Bytes raw(AdminKeyJournal::RecordSize, 0);
  raw[0] = 'Z'; raw[1] = 'T'; raw[2] = 'A'; raw[3] = 1;
  put32(raw.data() + 4, generation);
  raw[8] = settings.count();
  memcpy(raw.data() + AdminKeyJournal::HeaderSize, settings.packedKeys(),
         AdminKeySettings::PackedBytes);
  put32(raw.data() + raw.size() - AdminKeyJournal::CrcSize,
        crc32(raw.data(), raw.size() - AdminKeyJournal::CrcSize));
  return raw;
}

void reseal(Bytes &raw) {
  put32(raw.data() + raw.size() - AdminKeyJournal::CrcSize,
        crc32(raw.data(), raw.size() - AdminKeyJournal::CrcSize));
}

void freshDefaultIsDenyAllWithoutBootWrites() {
  MemoryBackend backend;
  AdminKeyJournal journal;
  assert(journal.state() == AdminKeyJournal::State::Unloaded);
  assert(journal.load(backend) == 0 && journal.ready());
  assert(journal.state() == AdminKeyJournal::State::Fresh &&
         journal.settings().count() == 0 && backend.writes == 0);

  meshtastic_Config_SecurityConfig security = meshtastic_Config_SecurityConfig_init_zero;
  security.admin_key_count = 2;
  security.admin_key[0].size = security.admin_key[1].size = 32;
  memset(security.admin_key[0].bytes, 0x33, 32);
  memset(security.admin_key[1].bytes, 0x44, 32);
  security.public_key.size = 32;
  security.private_key.size = 32;
  memset(security.public_key.bytes, 0x71, 32);
  memset(security.private_key.bytes, 0xa7, 32);
  security.is_managed = true;
  assert(journal.project(security));
  assert(security.admin_key_count == 0);
  for (const auto &entry : security.admin_key) {
    assert(entry.size == 0);
    for (uint8_t byte : entry.bytes) assert(byte == 0);
  }
  assert(security.public_key.size == 32 && security.private_key.size == 32 &&
         security.public_key.bytes[0] == 0x71 &&
         security.private_key.bytes[0] == 0xa7 && security.is_managed);
  assert(journal.save(backend, AdminKeySettings{}) == 0 && backend.writes == 0);
}

void roundTripRemovalAndProjectionAreTypedAndPublicOnly() {
  MemoryBackend backend;
  AdminKeyJournal journal;
  assert(journal.load(backend) == 0);
  const auto first = makeSettings({0x31});
  assert(journal.save(backend, first) == 0 && backend.writes == 1);
  assert(journal.generation() == 1 && journal.activeSlot() == 0);
  assert(backend.rows.at("mtadmin/state0").size() == AdminKeyJournal::RecordSize);
  assert(backend.rows.at("mtadmin/state0")[0] == 'Z' &&
         backend.rows.at("mtadmin/state0")[1] == 'T' &&
         backend.rows.at("mtadmin/state0")[2] == 'A');
  const auto &oneKeyRecord = backend.rows.at("mtadmin/state0");
  assert(oneKeyRecord[9] == 0 && oneKeyRecord[10] == 0 && oneKeyRecord[11] == 0);
  for (size_t i = AdminKeyJournal::HeaderSize + AdminKeySettings::KeyBytes;
       i < AdminKeyJournal::RecordSize - AdminKeyJournal::CrcSize; ++i)
    assert(oneKeyRecord[i] == 0);
  assert(std::find(backend.rows.at("mtadmin/state0").begin(),
                   backend.rows.at("mtadmin/state0").end(), uint8_t(0xa7)) ==
         backend.rows.at("mtadmin/state0").end());

  assert(journal.save(backend, first) == 0 && backend.writes == 1);
  const auto all = makeSettings({0x31, 0x52, 0x63});
  assert(journal.save(backend, all) == 0 && backend.writes == 2);
  assert(journal.generation() == 2 && journal.activeSlot() == 1);
  AdminKeyJournal reloaded;
  assert(reloaded.load(backend) == 0 && reloaded.ready() &&
         reloaded.settings().same(all) && reloaded.generation() == 2);

  meshtastic_Config_SecurityConfig security = meshtastic_Config_SecurityConfig_init_zero;
  security.private_key.size = 32;
  memset(security.private_key.bytes, 0x7a, 32);
  assert(reloaded.project(security));
  assert(security.admin_key_count == 3);
  assert(security.admin_key[0].size == 32 && security.admin_key[0].bytes[0] == 0x31);
  assert(security.admin_key[1].size == 32 && security.admin_key[1].bytes[0] == 0x52);
  assert(security.admin_key[2].size == 32 && security.admin_key[2].bytes[0] == 0x63);
  assert(security.private_key.size == 32 && security.private_key.bytes[0] == 0x7a);

  const AdminKeySettings removed;
  assert(reloaded.save(backend, removed) == 0 && backend.writes == 3);
  assert(reloaded.settings().count() == 0 && reloaded.generation() == 3);
  AdminKeyJournal afterRemoval;
  assert(afterRemoval.load(backend) == 0 && afterRemoval.settings().count() == 0 &&
         afterRemoval.generation() == 3);
  meshtastic_Config_SecurityConfig projected = meshtastic_Config_SecurityConfig_init_zero;
  assert(afterRemoval.project(projected) && projected.admin_key_count == 0);
}

void everyTruncatedPresentSlotRevokesDespiteAnotherValidSlot() {
  const auto allowed = makeSettings({0x31});
  const Bytes valid = record(4, allowed);
  for (size_t prefix = 0; prefix < AdminKeyJournal::RecordSize; ++prefix) {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = valid;
    backend.rows["mtadmin/state1"] = Bytes(valid.begin(), valid.begin() + prefix);
    AdminKeyJournal journal;
    assert(journal.load(backend) == -EINVAL);
    assert(journal.state() == AdminKeyJournal::State::Invalid && !journal.ready() &&
           journal.settings().count() == 0);
  }
}

void malformedAndOversizedPresentSlotsNeverFallback() {
  const auto allowed = makeSettings({0x31});
  const Bytes valid = record(4, allowed);
  const auto malformed = [&](const Bytes &bad) {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = valid;
    backend.rows["mtadmin/state1"] = bad;
    AdminKeyJournal journal;
    assert(journal.load(backend) == -EINVAL);
    assert(journal.state() == AdminKeyJournal::State::Invalid && !journal.ready());
  };

  auto bad = valid; bad[0] ^= 1; reseal(bad); malformed(bad);
  bad = valid; bad[3] = 2; reseal(bad); malformed(bad);
  bad = valid; bad[9] = 1; reseal(bad); malformed(bad);
  bad = valid; bad[8] = 4; reseal(bad); malformed(bad);
  bad = valid; bad[AdminKeyJournal::HeaderSize] = 0;
  std::fill(bad.begin() + AdminKeyJournal::HeaderSize,
            bad.begin() + AdminKeyJournal::HeaderSize + AdminKeySettings::KeyBytes, 0);
  reseal(bad); malformed(bad);
  bad = valid; bad[AdminKeyJournal::HeaderSize + AdminKeySettings::KeyBytes] = 0x55;
  reseal(bad); malformed(bad);  // Nonzero unused key slot.
  bad = valid; bad.back() ^= 1; malformed(bad);  // CRC mismatch.
  bad = valid; bad.push_back(0); malformed(bad); // Backend reports -E2BIG.
}

void readErrorsAndAmbiguousGenerationsFailClosed() {
  const auto first = makeSettings({0x31});
  const auto second = makeSettings({0x52});
  {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = record(7, first);
    backend.readError = -EIO;
    backend.readErrorKey = "mtadmin/state1";
    AdminKeyJournal journal;
    assert(journal.load(backend) == -EIO &&
           journal.state() == AdminKeyJournal::State::IoError &&
           journal.settings().count() == 0);
  }
  {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = record(7, first);
    backend.rows["mtadmin/state1"] = record(7, second);
    AdminKeyJournal journal;
    assert(journal.load(backend) == -EINVAL &&
           journal.state() == AdminKeyJournal::State::Invalid);
  }
  {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = record(9, first);
    backend.rows["mtadmin/state1"] = record(9, first);
    AdminKeyJournal journal;
    assert(journal.load(backend) == 0 && journal.settings().same(first) &&
           journal.activeSlot() == 0);
  }
  {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = record(0, first);
    backend.rows["mtadmin/state1"] = record(0x80000000u, second);
    AdminKeyJournal journal;
    assert(journal.load(backend) == -EINVAL &&
           journal.state() == AdminKeyJournal::State::Invalid);
  }
  {
    MemoryBackend backend;
    backend.rows["mtadmin/state0"] = record(UINT32_MAX, first);
    backend.rows["mtadmin/state1"] = record(0, second);
    AdminKeyJournal journal;
    assert(journal.load(backend) == 0 && journal.settings().same(second) &&
           journal.generation() == 0);
  }
}

void malformedAssignmentsAndUncertainWritesPoisonVolatileKeys() {
  AdminKeySettings invalid;
  std::array<uint8_t, AdminKeySettings::PackedBytes> packed{};
  assert(!invalid.assign(4, packed.data(), 4 * AdminKeySettings::KeyBytes));
  assert(!invalid.assign(1, packed.data(), 0));
  assert(!invalid.assign(1, nullptr, AdminKeySettings::KeyBytes));
  assert(!invalid.assign(1, packed.data(), AdminKeySettings::KeyBytes));  // all-zero key

  const auto candidate = makeSettings({0x31});
  for (int mode = 0; mode < 5; ++mode) {
    MemoryBackend backend;
    AdminKeyJournal journal;
    assert(journal.load(backend) == 0);
    if (mode == 0) backend.writeErrorBefore = -EIO;
    if (mode == 1) backend.writeErrorAfter = -EIO;
    if (mode == 2) backend.corruptReadback = true;
    if (mode == 3) backend.shortReadback = true;
    if (mode == 4) backend.readError = -EIO;
    const int rc = journal.save(backend, candidate);
    assert(rc < 0 && journal.state() == AdminKeyJournal::State::IoError &&
           !journal.ready() && journal.settings().count() == 0);
    meshtastic_Config_SecurityConfig security = meshtastic_Config_SecurityConfig_init_zero;
    security.admin_key_count = 1;
    security.admin_key[0].size = AdminKeySettings::KeyBytes;
    memset(security.admin_key[0].bytes, 0x31, AdminKeySettings::KeyBytes);
    assert(!journal.project(security) && security.admin_key_count == 0);
    assert(security.admin_key[0].size == 0 && security.admin_key[0].bytes[0] == 0);
    const unsigned writes = backend.writes;
    backend.writeErrorBefore = backend.writeErrorAfter = backend.readError = 0;
    backend.corruptReadback = backend.shortReadback = false;
    assert(journal.save(backend, candidate) == -EACCES && backend.writes == writes);
  }
}
}  // namespace

int main() {
  freshDefaultIsDenyAllWithoutBootWrites();
  roundTripRemovalAndProjectionAreTypedAndPublicOnly();
  everyTruncatedPresentSlotRevokesDespiteAnotherValidSlot();
  malformedAndOversizedPresentSlotsNeverFallback();
  readErrorsAndAmbiguousGenerationsFailClosed();
  malformedAssignmentsAndUncertainWritesPoisonVolatileKeys();
  std::puts("PASS M143 Admin key journal fail-closed public-key controls");
}
