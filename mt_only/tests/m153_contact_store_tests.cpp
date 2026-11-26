// SPDX-License-Identifier: GPL-3.0-only
// Host-only in-memory prototype tests. No flash/settings/device access.
#include "pki/PkiContactStore.h"
#include "monocypher.h"

#include <assert.h>
#include <errno.h>
#include <map>
#include <string>
#include <vector>
#include <string.h>
#include <stdio.h>

using namespace mt::pki;
using Bytes = std::vector<uint8_t>;

namespace {
struct Key {
  uint8_t scalar[32]{};
  IdentityBinding binding{};
  explicit Key(uint8_t seed) {
    for (unsigned i = 0; i < 32; ++i) scalar[i] = uint8_t(seed + 31u * i);
    scalar[0] &= 248;
    scalar[31] = uint8_t((scalar[31] & 127) | 64);
    crypto_x25519_public_key(binding.publicKey, scalar);
    binding.node = bench::MtSignature::nodeId(binding.publicKey);
    binding.generation = 1;
  }
  ~Key() { crypto_wipe(scalar, sizeof(scalar)); }
};

struct Memory final : mt::SettingsBackend {
  std::map<std::string, Bytes> rows;
  unsigned reads = 0, writes = 0;
  int failReadAt = -1, failWriteAt = -1, corruptReadAt = -1;
  bool corruptStoredOnRead = false;
  bool commitOnWriteError = false, tearOnWriteError = false;
  int read(const char *name, uint8_t *out, size_t cap) override {
    ++reads;
    if (int(reads) == failReadAt) return -EIO;
    auto it = rows.find(name);
    if (it == rows.end()) return -ENOENT;
    if (it->second.size() > cap) return -E2BIG;
    if (int(reads) == corruptReadAt && corruptStoredOnRead && !it->second.empty())
      it->second[0] ^= 1;
    memcpy(out, it->second.data(), it->second.size());
    if (int(reads) == corruptReadAt && !corruptStoredOnRead && !it->second.empty()) out[0] ^= 1;
    return int(it->second.size());
  }
  int write(const char *name, const uint8_t *data, size_t size) override {
    ++writes;
    if (int(writes) == failWriteAt) {
      if (commitOnWriteError) rows[name] = Bytes(data, data + size);
      else if (tearOnWriteError) rows[name] = Bytes(data, data + size / 2);
      return -EIO;
    }
    rows[name] = Bytes(data, data + size);
    return 0;
  }
};

void copyPin(PkiPeerPin &pin, const IdentityBinding &binding) {
  pin.node = binding.node;
  memcpy(pin.publicKey, binding.publicKey, 32);
}
bool contains(const PkiContactSnapshot::Value &value, const PkiPeerPin &pin) {
  for (size_t i = 0; i < value.count; ++i)
    if (value.contacts[i].node == pin.node &&
        !memcmp(value.contacts[i].publicKey, pin.publicKey, 32)) return true;
  return false;
}
bool same(const PkiContactSnapshot::Value &a, const PkiContactSnapshot::Value &b) {
  if (a.generation != b.generation || a.localIdentityGeneration != b.localIdentityGeneration ||
      a.legacyRootGeneration != b.legacyRootGeneration || a.legacyRootCrc != b.legacyRootCrc ||
      a.count != b.count) return false;
  for (size_t i = 0; i < a.count; ++i)
    if (a.contacts[i].node != b.contacts[i].node ||
        memcmp(a.contacts[i].publicKey, b.contacts[i].publicKey, 32)) return false;
  return true;
}
bool snapshot(const PkiContactStore &store, PkiContactSnapshot::Value &out) {
  return store.snapshot(out);
}
void encodeRecord(const IdentityBinding &local, uint32_t gen,
                  const PkiPeerPin *pins, size_t count, Bytes &out) {
  out.resize(PkiContactStore::RecordBytes);
  assert(PkiContactSnapshot::encode(local, gen, 9, 0x76543210u,
                                     pins, count, out.data(), out.size()));
}

void emptyLoadAndExplicitGenesis() {
  Key local(7), a(21), b(22);
  Memory mem;
  PkiContactStore store;
  assert(!PkiContactStore::key(0) || strcmp(PkiContactStore::key(0), PkiContactStore::key(1)) != 0);
  assert(strcmp(PkiContactStore::key(0), "mtpki/contacts0") == 0);
  assert(strcmp(PkiContactStore::key(1), "mtpki/contacts1") == 0);
  assert(PkiContactStore::key(2) == nullptr); // invalid slot is null
  const char *existing[] = {"mtidentity/state0", "mtidentity/state1", "mtpki/ledger0",
                            "mtpki/ledger1", "mtpki/nonces0", "mtpki/nonces1"};
  for (const char *name : existing) {
    assert(strcmp(PkiContactStore::key(0), name) != 0);
    assert(strcmp(PkiContactStore::key(1), name) != 0);
  }
  assert(store.load(mem, local.binding, 9, 0x76543210u) == 0);
  assert(store.state() == PkiContactStore::State::Empty && mem.writes == 0);
  Memory stale;
  PkiContactStore staleStore;
  assert(staleStore.load(stale, local.binding, 9, 0x76543210u) == 0);
  stale.rows[PkiContactStore::key(1)] = Bytes(PkiContactStore::RecordBytes, 0);
  PkiPeerPin one{}; copyPin(one, a.binding);
  assert(staleStore.initialize(stale, &one, 1) == -EEXIST);
  assert(stale.writes == 0 && staleStore.state() == PkiContactStore::State::Frozen);
  PkiPeerPin pins[2]{}; copyPin(pins[0], a.binding); copyPin(pins[1], b.binding);
  assert(store.initialize(mem, pins, 17) == -EINVAL && mem.writes == 0);
  pins[1] = pins[0];
  assert(store.initialize(mem, pins, 2) == -EINVAL && mem.writes == 0);
  copyPin(pins[1], b.binding);
  assert(store.initialize(mem, pins, 2) == 0);
  assert(store.ready() && store.generation() == 2 && store.activeSlot() == 1);
  assert(mem.writes == 2 && mem.rows.size() == 2);
  PkiContactSnapshot::Value value{};
  assert(snapshot(store, value) && value.generation == 2 && value.count == 2);
}

void fullCapacityRoundTrip() {
  Key local(25);
  Key keys[16] = {Key(40),Key(41),Key(42),Key(43),Key(44),Key(45),Key(46),Key(47),
                  Key(48),Key(49),Key(50),Key(51),Key(52),Key(53),Key(54),Key(55)};
  PkiPeerPin contacts[16]{};
  for (size_t i = 0; i < 16; ++i) copyPin(contacts[i], keys[i].binding);
  Memory mem;
  PkiContactStore store;
  assert(store.load(mem, local.binding, 9, 0x76543210u) == 0);
  assert(store.initialize(mem, contacts, 16) == 0);
  PkiContactSnapshot::Value value{};
  assert(snapshot(store, value) && value.count == 16);
  for (size_t i = 0; i < 16; ++i) assert(contains(value, contacts[i]));
  const unsigned writes = mem.writes;
  assert(store.replace(mem, contacts, 17) == -EINVAL);
  assert(mem.writes == writes && store.generation() == 2);
}

void removeAndSameKeyReaddRoundTrip() {
  Key local(31), a(32), b(33), c(34);
  PkiPeerPin all[3]{}; copyPin(all[0], a.binding); copyPin(all[1], b.binding); copyPin(all[2], c.binding);
  Memory mem;
  PkiContactStore store;
  assert(store.load(mem, local.binding, 9, 0x76543210u) == 0);
  assert(store.initialize(mem, all, 3) == 0);
  PkiContactSnapshot::Value before{};
  assert(snapshot(store, before) && before.generation == 2 && contains(before, all[1]));

  PkiPeerPin removed[2]{}; removed[0] = all[0]; removed[1] = all[2];
  assert(store.replace(mem, removed, 2) == 0);
  PkiContactSnapshot::Value afterRemove{};
  assert(snapshot(store, afterRemove) && afterRemove.generation == 3 &&
         !contains(afterRemove, all[1]) && contains(afterRemove, all[0]));
  assert(store.replace(mem, all, 3) == 0);
  PkiContactSnapshot::Value afterReadd{};
  assert(snapshot(store, afterReadd) && afterReadd.generation == 4 &&
         contains(afterReadd, all[1]) && contains(afterReadd, all[2]));

  PkiContactStore reopened;
  unsigned writes = mem.writes;
  assert(reopened.load(mem, local.binding, 9, 0x76543210u) == 0);
  PkiContactSnapshot::Value restored{};
  assert(snapshot(reopened, restored) && same(restored, afterReadd));
  assert(mem.writes == writes);
}

void noPublishBeforeExactReadback() {
  Key local(41), peer(42);
  PkiPeerPin pin{}; copyPin(pin, peer.binding);
  Memory mem;
  PkiContactStore store;
  assert(store.load(mem, local.binding, 9, 0x76543210u) == 0);
  // load plus initialization preflight consumed reads 1-4; fail the second exact genesis readback.
  mem.corruptReadAt = 6;
  mem.corruptStoredOnRead = true;
  assert(store.initialize(mem, &pin, 1) == -EIO);
  assert(store.state() == PkiContactStore::State::Frozen && !store.ready());
  PkiContactSnapshot::Value sentinel{}; sentinel.generation = 123; sentinel.count = 7;
  auto before = sentinel;
  assert(!snapshot(store, sentinel) && same(sentinel, before));

  // The partially initialized pair is never repaired by a fresh read.
  PkiContactStore reopened;
  unsigned writes = mem.writes;
  assert(reopened.load(mem, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(mem.writes == writes && reopened.state() == PkiContactStore::State::Frozen);

  Memory secondMem;
  PkiContactStore second;
  assert(second.load(secondMem, local.binding, 9, 0x76543210u) == 0);
  assert(second.initialize(secondMem, &pin, 1) == 0);
  PkiPeerPin empty[1]{};
  secondMem.corruptReadAt = int(secondMem.reads + 1);
  secondMem.corruptStoredOnRead = true;
  assert(second.replace(secondMem, empty, 0) == -EIO);
  assert(second.state() == PkiContactStore::State::Frozen);
  assert(!snapshot(second, sentinel) && same(sentinel, before));
}

void writeAndReadFailuresFreeze() {
  Key local(50), peer(51);
  PkiPeerPin pin{}; copyPin(pin, peer.binding);
  Memory mem;
  PkiContactStore store;
  assert(store.load(mem, local.binding, 9, 0x76543210u) == 0);
  mem.failWriteAt = 1; mem.tearOnWriteError = true;
  assert(store.initialize(mem, &pin, 1) == -EIO);
  assert(store.state() == PkiContactStore::State::Frozen && !store.ready());
  PkiContactStore fresh;
  const unsigned writes = mem.writes;
  assert(fresh.load(mem, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(mem.writes == writes);

  Memory mem2;
  PkiContactStore good;
  assert(good.load(mem2, local.binding, 9, 0x76543210u) == 0);
  assert(good.initialize(mem2, &pin, 1) == 0);
  mem2.failWriteAt = int(mem2.writes + 1); mem2.tearOnWriteError = true;
  assert(good.replace(mem2, nullptr, 0) == -EIO);
  assert(good.state() == PkiContactStore::State::Frozen);
  PkiContactStore reread;
  const unsigned writes2 = mem2.writes;
  assert(reread.load(mem2, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(mem2.writes == writes2);

  Memory readError;
  PkiContactStore readStore;
  assert(readStore.load(readError, local.binding, 9, 0x76543210u) == 0);
  assert(readStore.initialize(readError, &pin, 1) == 0);
  readError.failReadAt = int(readError.reads + 1);
  assert(readStore.replace(readError, &pin, 1) == -EIO);
  assert(readStore.state() == PkiContactStore::State::Frozen);
}

void corruptPartialAndBindingReject() {
  Key local(60), peer(61);
  PkiPeerPin pin{}; copyPin(pin, peer.binding);
  Bytes rec1, rec2;
  encodeRecord(local.binding, 1, &pin, 1, rec1);
  encodeRecord(local.binding, 2, &pin, 1, rec2);

  Memory single;
  single.rows[PkiContactStore::key(0)] = rec1;
  PkiContactStore one;
  assert(one.load(single, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(single.writes == 0);

  Memory loneSlot1;
  loneSlot1.rows[PkiContactStore::key(1)] = rec1;
  PkiContactStore oneAtOne;
  assert(oneAtOne.load(loneSlot1, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(loneSlot1.writes == 0);

  Memory gap;
  encodeRecord(local.binding, 4, &pin, 1, rec1);
  encodeRecord(local.binding, 6, &pin, 1, rec2);
  gap.rows[PkiContactStore::key(0)] = rec1; gap.rows[PkiContactStore::key(1)] = rec2;
  PkiContactStore nonAdjacent;
  assert(nonAdjacent.load(gap, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(gap.writes == 0);

  Memory wrongIdentity;
  encodeRecord(local.binding, 1, &pin, 1, rec1);
  encodeRecord(local.binding, 2, &pin, 1, rec2);
  wrongIdentity.rows[PkiContactStore::key(0)] = rec1; wrongIdentity.rows[PkiContactStore::key(1)] = rec2;
  PkiContactStore wrong;
  IdentityBinding changed = local.binding; ++changed.generation;
  assert(wrong.load(wrongIdentity, changed, 9, 0x76543210u) == -EILSEQ);
  assert(wrongIdentity.writes == 0);
  PkiContactStore wrongRoot;
  assert(wrongRoot.load(wrongIdentity, local.binding, 10, 0x76543210u) == -EILSEQ);
  assert(wrongIdentity.writes == 0);

  Memory corrupted;
  rec1[100] ^= 1;
  corrupted.rows[PkiContactStore::key(0)] = rec1;
  encodeRecord(local.binding, 2, &pin, 1, rec2);
  corrupted.rows[PkiContactStore::key(1)] = rec2;
  PkiContactStore bad;
  assert(bad.load(corrupted, local.binding, 9, 0x76543210u) == -EILSEQ);
  assert(corrupted.writes == 0);
}

void generationWrapAndInvalidReplaceDoNotWrite() {
  Key local(71), a(72), b(73);
  PkiPeerPin pin{}; copyPin(pin, a.binding);
  Bytes hi, max;
  encodeRecord(local.binding, UINT32_MAX - 1, &pin, 1, hi);
  encodeRecord(local.binding, UINT32_MAX, &pin, 1, max);
  Memory mem; mem.rows[PkiContactStore::key(0)] = hi; mem.rows[PkiContactStore::key(1)] = max;
  PkiContactStore store;
  assert(store.load(mem, local.binding, 9, 0x76543210u) == 0);
  PkiContactSnapshot::Value before{}; assert(snapshot(store, before));
  unsigned writes = mem.writes;
  assert(store.replace(mem, &pin, 1) == -EOVERFLOW);
  assert(mem.writes == writes);

  assert(store.generation() == UINT32_MAX);

  Memory regular;
  PkiContactStore normal;
  assert(normal.load(regular, local.binding, 9, 0x76543210u) == 0);
  assert(normal.initialize(regular, &pin, 1) == 0);
  PkiContactSnapshot::Value old{}; assert(snapshot(normal, old));
  PkiPeerPin self{}; copyPin(self, local.binding); writes = regular.writes;
  assert(normal.replace(regular, &self, 1) == -EINVAL);
  assert(regular.writes == writes);
  PkiPeerPin invalidRows[2]{}; copyPin(invalidRows[0], a.binding); copyPin(invalidRows[1], b.binding);
  invalidRows[1] = invalidRows[0]; writes = regular.writes;
  assert(normal.replace(regular, invalidRows, 2) == -EINVAL);
  assert(regular.writes == writes);
  PkiContactSnapshot::Value after{}; assert(snapshot(normal, after) && same(old, after));
}
} // namespace

int main() {
  emptyLoadAndExplicitGenesis();
  fullCapacityRoundTrip();
  removeAndSameKeyReaddRoundTrip();
  noPublishBeforeExactReadback();
  writeAndReadFailuresFreeze();
  corruptPartialAndBindingReject();
  generationWrapAndInvalidReplaceDoNotWrite();
  puts("PASS M153 in-memory two-slot contact store: explicit adjacent genesis, bounded membership snapshots, exact readback, and fail-closed errors");
  return 0;
}
