// SPDX-License-Identifier: GPL-3.0-only
#include "modules/StoreForwardJournal.h"

#include <assert.h>
#include <errno.h>
#include <map>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace mt;
using namespace mt::modules;
using Bytes = std::vector<uint8_t>;

struct Backend final : SettingsBackend {
  enum class Fault { None, BeforeWrite, CommittedError, TornError, TornSuccess, ReadbackError,
                     ReadbackShort, ReadbackDifferent };
  std::map<std::string, Bytes> records;
  std::vector<std::string> reads;
  std::vector<std::string> writes;
  Fault fault = Fault::None;
  bool afterWrite = false;

  int read(const char *key, uint8_t *out, size_t capacity) override {
    reads.emplace_back(key);
    if (fault == Fault::ReadbackError && afterWrite) return -EIO;
    const auto found = records.find(key);
    if (found == records.end()) return -ENOENT;
    if (found->second.size() > capacity) return -E2BIG;
    const size_t size = found->second.size();
    if (size) memcpy(out, found->second.data(), size);
    if (afterWrite && fault == Fault::ReadbackDifferent && size)
      out[size - 1] ^= 1;
    if (afterWrite && fault == Fault::ReadbackShort && size)
      return int(size - 1);
    return int(size);
  }

  int write(const char *key, const uint8_t *data, size_t size) override {
    writes.emplace_back(key);
    afterWrite = true;
    if (fault == Fault::BeforeWrite) return -EIO;
    records[key] = Bytes(data, data + size);
    if (fault == Fault::TornError) records[key].resize(17);
    if (fault == Fault::TornSuccess) records[key].resize(17);
    if (fault == Fault::CommittedError) return -EIO;
    if (fault == Fault::TornError) return -EIO;
    return 0;
  }
};

static unsigned groups = 0;
static void pass(const char *name) {
  ++groups;
  printf("PASS %s\n", name);
}
static uint32_t crc32(const uint8_t *p, size_t size) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < size; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & uint32_t(-int32_t(crc & 1)));
  }
  return ~crc;
}
static uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void put32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
static void sealOuter(Bytes &record) {
  assert(record.size() == StoreForwardJournal::RecordSize);
  put32(record.data() + record.size() - 4,
        crc32(record.data(), record.size() - 4));
}
static meshtastic_ModuleConfig_StoreForwardConfig config(
    bool enabled, bool heartbeat, bool server, uint32_t records,
    uint32_t maximum, uint32_t window) {
  meshtastic_ModuleConfig_StoreForwardConfig out{};
  out.enabled = enabled;
  out.heartbeat = heartbeat;
  out.is_server = server;
  out.records = records;
  out.history_return_max = maximum;
  out.history_return_window = window;
  return out;
}
static StoreForwardSettings settings(
    const meshtastic_ModuleConfig_StoreForwardConfig &value) {
  StoreForwardSettings out;
  assert(out.set(value) && out.valid());
  return out;
}
static void rawDefaultsAndRuntimeDefaults() {
  StoreForwardSettings defaults;
  assert(defaults.valid());
  meshtastic_ModuleConfig_StoreForwardConfig raw{};
  assert(defaults.get(raw));
  assert(!raw.enabled && !raw.heartbeat && !raw.is_server && !raw.records &&
         !raw.history_return_max && !raw.history_return_window);
  const auto runtime = defaults.normalized();
  assert(!runtime.enabled && !runtime.heartbeat && !runtime.isServer);
  assert(runtime.records == 400 && runtime.historyReturnMax == 25 &&
         runtime.historyReturnWindow == 240);

  uint8_t encoded[StoreForwardSettings::Size];
  assert(defaults.encode(encoded, sizeof(encoded)));
  assert(encoded[0] == 'Z' && encoded[1] == 'T' && encoded[2] == 'F' &&
         encoded[3] == StoreForwardSettings::Version && encoded[4] == 0);
  for (size_t i = 8; i < 20; ++i) assert(encoded[i] == 0);
  StoreForwardSettings decoded;
  assert(StoreForwardSettings::decode(encoded, sizeof(encoded), decoded));
  assert(decoded.same(defaults));
  pass("raw zero settings remain zero while runtime alone applies stock defaults");
}

static void fullRoundTripAndAtomicValidation() {
  StoreForwardSettings value;
  const auto all = config(true, true, true, 400, 400, 71582);
  assert(value.set(all));
  meshtastic_ModuleConfig_StoreForwardConfig out{};
  assert(value.get(out));
  assert(out.enabled && out.heartbeat && out.is_server && out.records == 400 &&
         out.history_return_max == 400 && out.history_return_window == 71582);

  const auto before = value;
  const meshtastic_ModuleConfig_StoreForwardConfig invalid[] = {
      config(true, false, true, 401, 1, 1),
      config(true, false, true, 1, 401, 1),
      config(true, false, true, 1, 1, 71583),
  };
  for (const auto &candidate : invalid) {
    assert(!value.set(candidate));
    assert(value.same(before));
  }
  pass("all six raw fields roundtrip and limits reject atomically at 400/71582");
}

static void journalKeysRoundtripAndLoadIsReadOnly() {
  Backend backend;
  backend.records["mtbase/state0"] = {1, 2, 3};
  backend.records["mtmod/state0"] = {4, 5, 6};
  backend.records["mtidentity/state0"] = {7, 8, 9};
  const auto unrelated = backend.records;
  StoreForwardJournal journal;
  assert(!journal.ready() && journal.load(backend) == 0);
  assert(journal.state() == StoreForwardJournal::State::Fresh &&
         journal.generation() == 0 && journal.activeSlot() == -1);
  assert(backend.writes.empty());
  assert(backend.reads ==
         std::vector<std::string>({"mtsf/state0", "mtsf/state1"}));
  meshtastic_ModuleConfig_StoreForwardConfig raw{};
  assert(journal.config().get(raw));
  assert(!raw.enabled && !raw.heartbeat && !raw.is_server && !raw.records &&
         !raw.history_return_max && !raw.history_return_window);

  const auto first = settings(config(true, true, false, 321, 17, 900));
  assert(journal.save(backend, first) == 0);
  assert(journal.state() == StoreForwardJournal::State::Loaded &&
         journal.generation() == 1 && journal.activeSlot() == 0);
  const auto &record = backend.records.at("mtsf/state0");
  assert(record.size() == 40 && record[0] == 'Z' && record[1] == 'T' &&
         record[2] == 'S' && record[3] == 1 && get32(record.data() + 4) == 1);
  assert(record[8] == StoreForwardSettings::Size && !record[9] && !record[10] &&
         !record[11]);
  assert(get32(record.data() + 36) == crc32(record.data(), 36));

  const size_t readsBeforeReload = backend.reads.size();
  const size_t writesBeforeReload = backend.writes.size();
  StoreForwardJournal reload;
  assert(reload.load(backend) == 0 &&
         reload.state() == StoreForwardJournal::State::Loaded &&
         reload.generation() == 1 && reload.config().same(first));
  assert(backend.writes.size() == writesBeforeReload);
  assert(backend.reads.size() == readsBeforeReload + 2);
  assert(backend.records.at("mtbase/state0") == unrelated.at("mtbase/state0") &&
         backend.records.at("mtmod/state0") == unrelated.at("mtmod/state0") &&
         backend.records.at("mtidentity/state0") == unrelated.at("mtidentity/state0"));
  for (const auto &key : backend.reads)
    assert(key == "mtsf/state0" || key == "mtsf/state1");
  for (const auto &key : backend.writes)
    assert(key == "mtsf/state0" || key == "mtsf/state1");

  const auto second = settings(config(false, true, true, 400, 25, 240));
  assert(reload.save(backend, second) == 0 && reload.generation() == 2 &&
         reload.activeSlot() == 1);
  StoreForwardJournal latest;
  assert(latest.load(backend) == 0 && latest.config().same(second));
  pass("40-byte journal roundtrip alternates slots and touches only mtsf keys");
}

static void recordValidationAndPartialRecovery() {
  Backend seed;
  StoreForwardJournal writer;
  assert(writer.load(seed) == 0);
  const auto old = settings(config(false, false, false, 0, 0, 0));
  const auto current = settings(config(true, false, true, 200, 10, 60));
  assert(writer.save(seed, old) == 0 && writer.save(seed, current) == 0);
  const Bytes valid = seed.records.at("mtsf/state1");

  for (size_t cut : {size_t(0), size_t(1), size_t(12), size_t(23), size_t(39)}) {
    Backend partial;
    partial.records["mtsf/state0"] = valid;
    partial.records["mtsf/state1"] = valid;
    partial.records["mtsf/state1"].resize(cut);
    const auto before = partial.records;
    StoreForwardJournal recovered;
    assert(recovered.load(partial) == 0 &&
           recovered.state() == StoreForwardJournal::State::Recovered &&
           recovered.config().same(current));
    assert(partial.writes.empty() && partial.records == before);
  }

  for (size_t cut : {size_t(0), size_t(1), size_t(12), size_t(23), size_t(39)}) {
    Backend onlyPartial;
    onlyPartial.records["mtsf/state0"] = valid;
    onlyPartial.records["mtsf/state0"].resize(cut);
    StoreForwardJournal invalid;
    assert(invalid.load(onlyPartial) == -EINVAL &&
           invalid.state() == StoreForwardJournal::State::Invalid &&
           !invalid.ready() && onlyPartial.writes.empty());
  }
  Backend oversized;
  oversized.records["mtsf/state0"] = valid;
  oversized.records["mtsf/state0"].push_back(0);
  StoreForwardJournal tooLong;
  assert(tooLong.load(oversized) == -EINVAL &&
         tooLong.state() == StoreForwardJournal::State::Invalid);

  Backend corruptInner;
  corruptInner.records["mtsf/state0"] = valid;
  corruptInner.records["mtsf/state0"][12 + 8] ^= 1;  // inner records field
  sealOuter(corruptInner.records["mtsf/state0"]);
  StoreForwardJournal badInner;
  assert(badInner.load(corruptInner) == -EINVAL &&
         badInner.state() == StoreForwardJournal::State::Invalid);

  Backend corruptOuter;
  corruptOuter.records["mtsf/state0"] = valid;
  corruptOuter.records["mtsf/state0"][20] ^= 1;
  StoreForwardJournal badOuter;
  assert(badOuter.load(corruptOuter) == -EINVAL &&
         badOuter.state() == StoreForwardJournal::State::Invalid);

  // A recovered config identical to the good slot must still write the bad
  // slot as an explicit repair rather than being elided as unchanged.
  Backend repair = seed;
  repair.records["mtsf/state0"].resize(10);
  StoreForwardJournal recovered;
  assert(recovered.load(repair) == 0 &&
         recovered.state() == StoreForwardJournal::State::Recovered &&
         recovered.config().same(current));
  const size_t beforeRepair = repair.writes.size();
  assert(recovered.save(repair, current) == 0 &&
         repair.writes.size() == beforeRepair + 1 &&
         recovered.state() == StoreForwardJournal::State::Loaded &&
         repair.records.at("mtsf/state0").size() == StoreForwardJournal::RecordSize);
  pass("partial/corrupt records fail closed or recover read-only; equal recovered state repairs");
}

static void generationAmbiguityAndWrap() {
  Backend seed;
  StoreForwardJournal writer;
  assert(writer.load(seed) == 0);
  const auto a = settings(config(true, false, true, 10, 5, 60));
  const auto b = settings(config(true, true, true, 11, 6, 61));
  assert(writer.save(seed, a) == 0 && writer.save(seed, b) == 0);

  Backend sameGeneration = seed;
  put32(sameGeneration.records["mtsf/state1"].data() + 4, 1);
  sealOuter(sameGeneration.records["mtsf/state1"]);
  StoreForwardJournal same;
  assert(same.load(sameGeneration) == -EINVAL &&
         same.state() == StoreForwardJournal::State::Invalid);

  Backend halfRange = seed;
  put32(halfRange.records["mtsf/state1"].data() + 4, 0x80000001u);
  sealOuter(halfRange.records["mtsf/state1"]);
  StoreForwardJournal half;
  assert(half.load(halfRange) == -EINVAL &&
         half.state() == StoreForwardJournal::State::Invalid);

  Backend wrap;
  StoreForwardJournal wrapWriter;
  assert(wrapWriter.load(wrap) == 0 && wrapWriter.save(wrap, a) == 0);
  put32(wrap.records["mtsf/state0"].data() + 4, UINT32_MAX);
  sealOuter(wrap.records["mtsf/state0"]);
  const auto newer = settings(config(false, true, true, 12, 7, 62));
  // Generate a valid second slot, then make its generation wrap to zero.
  assert(wrapWriter.save(wrap, newer) == 0);
  put32(wrap.records["mtsf/state1"].data() + 4, 0);
  sealOuter(wrap.records["mtsf/state1"]);
  StoreForwardJournal wrapped;
  assert(wrapped.load(wrap) == 0 && wrapped.activeSlot() == 1 &&
         wrapped.generation() == 0 && wrapped.config().same(newer));
  pass("same-generation conflicts and half-range fail; wrap-safe newest slot selected");
}

static void uncertainWritesPoisonUntilReload() {
  const auto value = settings(config(true, true, true, 100, 25, 240));
  for (Backend::Fault fault : {Backend::Fault::BeforeWrite,
                               Backend::Fault::CommittedError,
                               Backend::Fault::TornError,
                               Backend::Fault::TornSuccess,
                               Backend::Fault::ReadbackError,
                               Backend::Fault::ReadbackShort,
                               Backend::Fault::ReadbackDifferent}) {
    Backend backend;
    StoreForwardJournal journal;
    assert(journal.load(backend) == 0);
    backend.fault = fault;
    assert(journal.save(backend, value) != 0 &&
           journal.state() == StoreForwardJournal::State::IoError);
    const size_t writes = backend.writes.size();
    assert(journal.save(backend, value) == -EACCES &&
           backend.writes.size() == writes);
  }
  Backend tornSuccess;
  StoreForwardJournal torn;
  assert(torn.load(tornSuccess) == 0);
  tornSuccess.fault = Backend::Fault::TornError;
  assert(torn.save(tornSuccess, value) == -EIO &&
         torn.state() == StoreForwardJournal::State::IoError);
  pass("write errors and uncertain readback poison saves until a fresh load");
}

int main() {
  rawDefaultsAndRuntimeDefaults();
  fullRoundTripAndAtomicValidation();
  journalKeysRoundtripAndLoadIsReadOnly();
  recordValidationAndPartialRecovery();
  generationAmbiguityAndWrap();
  uncertainWritesPoisonUntilReload();
  printf("RESULT PASS groups=%u\n", groups);
}
