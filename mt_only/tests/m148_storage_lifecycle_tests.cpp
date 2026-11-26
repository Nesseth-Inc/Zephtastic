// SPDX-License-Identifier: GPL-3.0-only
// Full M148 storage composition over synthetic NVS and NOR only.
#include "pki/PkiAppendHistory.h"
#include "pki/PkiNonceCounter.h"
#include "pki/PkiStorage.h"
#include "MtSignature.h"
#include "monocypher.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace mt::pki;
using Bytes = std::vector<uint8_t>;
using Values = std::map<std::string, Bytes>;

namespace {
constexpr uint32_t LegacyTxBase = 0x1001;
constexpr uint32_t LegacyRxBase = 0x2001;
constexpr uint32_t LegacyTxAppend = 0x3001;
constexpr uint32_t LegacyRxAppend = 0x4001;
constexpr uint32_t FillIdBase = 0x90000000u;
constexpr size_t SelectorOffset = 4092;

struct Key {
  uint8_t scalar[32]{};
  IdentityBinding identity{};
  explicit Key(unsigned seed = 1) {
    for (unsigned i = 0; i < sizeof(scalar); ++i)
      scalar[i] = uint8_t(seed + 23u * i);
    scalar[0] &= 248;
    scalar[31] = uint8_t((scalar[31] & 127) | 64);
    crypto_x25519_public_key(identity.publicKey, scalar);
    identity.node = bench::MtSignature::nodeId(identity.publicKey);
    identity.generation = 1;
  }
  Key(const Key &) = delete;
  Key &operator=(const Key &) = delete;
  ~Key() { crypto_wipe(scalar, sizeof(scalar)); }
  IdentityCandidate candidate() const { return IdentityCandidate(identity, scalar); }
};

struct Settings final : mt::SettingsBackend {
  enum class Fault { None, Before, After };
  Values values;
  unsigned reads = 0, writes = 0;
  unsigned failWriteNumber = 0;
  Fault fault = Fault::None;
  std::function<void()> writeGuard;

  int read(const char *name, uint8_t *out, size_t capacity) override {
    ++reads;
    if (!name || !out) return -EINVAL;
    const auto it = values.find(name);
    if (it == values.end()) return -ENOENT;
    if (it->second.size() > capacity) return -E2BIG;
    if (!it->second.empty()) memcpy(out, it->second.data(), it->second.size());
    return int(it->second.size());
  }
  int write(const char *name, const uint8_t *data, size_t size) override {
    ++writes;
    if (writeGuard) writeGuard();
    if (!name || !data || !size) return -EINVAL;
    const bool fail = failWriteNumber && writes == failWriteNumber;
    if (fail && fault == Fault::Before) return -EIO;
    values[name] = Bytes(data, data + size);
    return fail && fault == Fault::After ? -EIO : 0;
  }
};

struct Boot final : BootSettings {
  Settings &settings;
  bool held = false;
  unsigned begins = 0, ends = 0;
  explicit Boot(Settings &s) : settings(s) {}
  int begin() override {
    if (held) return -EBUSY;
    held = true;
    ++begins;
    return 0;
  }
  void end() override {
    assert(held);
    held = false;
    ++ends;
  }
  int read(const char *name, uint8_t *out, size_t size) override {
    return held ? settings.read(name, out, size) : -EACCES;
  }
};

struct Context final : PkiStorageContext {
  unsigned depth = 0;
  bool bootReady = true;
  bool paused = false;
  bool tryEnter() override {
    if (depth) return false;
    depth = 1;
    return true;
  }
  void leave() override { assert(depth == 1); depth = 0; }
  bool bootQuiescent() const override { return depth == 1 && bootReady; }
  bool quiescent() const override { return depth == 1 && paused; }
};

struct Entropy final : EntropySource {
  uint32_t next = 0x31415926u;
  unsigned calls = 0;
  bool fill(uint8_t *out, size_t size) override {
    ++calls;
    if (!out || size != 4) return false;
    const uint32_t value = next++;
    for (unsigned i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8 * i));
    return true;
  }
};

struct Nor final : AppendStore {
  enum class SelectorFault { None, Before, After, CorruptAfter };
  Bytes bytes = Bytes(PkiAppendHistory::StoreBytes, 0xff);
  Context &context;
  unsigned reads = 0, programs = 0, selectorAttempts = 0, selectorPrograms = 0;
  unsigned eraseCalls = 0;
  bool selectorAttempted = false;
  SelectorFault selectorFault = SelectorFault::None;
  explicit Nor(Context &c) : context(c) {}

  size_t capacity() const override { return bytes.size(); }
  int read(size_t offset, uint8_t *out, size_t size) override {
    ++reads;
    if (!out || offset > bytes.size() || size > bytes.size() - offset) return -EINVAL;
    if (size) memcpy(out, bytes.data() + offset, size);
    return 0;
  }
  int program(size_t offset, const uint8_t *data, size_t size) override {
    ++programs;
    assert(context.depth == 1 && context.paused);
    if (!data || !size || offset > bytes.size() || size > bytes.size() - offset)
      return -EINVAL;
    for (size_t i = 0; i < size; ++i)
      if ((bytes[offset + i] & data[i]) != data[i]) return -EEXIST;
    for (size_t i = 0; i < size; ++i) bytes[offset + i] &= data[i];
    return 0;
  }
  int selectCounterMode() override {
    ++selectorAttempts;
    if (selectorAttempted) return -EALREADY;
    selectorAttempted = true;
    assert(context.depth == 1 && context.paused);
    if (memcmp(bytes.data() + SelectorOffset, "CMIT", 4)) return -EILSEQ;
    if (selectorFault == SelectorFault::Before) return -EIO;
    ++selectorPrograms;
    if (selectorFault == SelectorFault::CorruptAfter) {
      // C(0x43)->A(0x41) is a legal one-way NOR write, but an invalid selector.
      bytes[SelectorOffset] = 'A';
      return -EIO;
    }
    bytes[SelectorOffset] = 'B'; // Exact one-bit CMIT -> BMIT transition.
    return selectorFault == SelectorFault::After ? -EIO : 0;
  }
};

struct Durable {
  Values settings;
  Bytes flash;
};

struct Rig {
  Key local{17};
  Key peers[6] = {Key{37}, Key{57}, Key{77}, Key{97}, Key{117}, Key{137}};
  Settings settings;
  Boot boot{settings};
  Context context;
  Entropy entropy;
  Nor nor{context};
  PkiAppendHistory append{nor};
  PkiStorage storage{boot, settings, context, entropy, &append};
  MonocypherIdentityDeriver deriver;

  Rig() = default;
  explicit Rig(const Durable &d) {
    settings.values = d.settings;
    nor.bytes = d.flash;
    bindGuards();
  }
  Rig(const Rig &) = delete;
  Rig &operator=(const Rig &) = delete;
  ~Rig() { (void)storage.close(); }

  void provisionLegacy() {
    IdentityJournal identity;
    PkiLedger ledger;
    assert(identity.load(settings, deriver) == 0);
    auto candidate = local.candidate();
    assert(identity.store(settings, deriver, candidate, true) == 0);
    assert(ledger.load(settings) == 0);
    assert(ledger.initialize(settings, local.identity.publicKey, local.identity.node, true) == 0);
    for (unsigned i = 0; i < 4; ++i)
      assert(ledger.addTrustedPeer(settings, peers[i].identity.node,
                                   peers[i].identity.publicKey, true) == 0);
    assert(ledger.reserve(settings, LegacyTxBase, true) == 0);
    for (unsigned i = 0; i < 4; ++i)
      assert(ledger.commitAuthenticated(settings, peers[i].identity.node,
                 peers[i].identity.publicKey, LegacyRxBase + i, true) == 0);
    assert(identity.close() && ledger.close());
    bindGuards();
  }

  void bindGuards() {
    settings.writeGuard = [this] { assert(context.depth == 1 && context.paused); };
  }

  void prepareActive(size_t expectedPins = 4) {
    assert(storage.prepare() == 0);
    assert(boot.begins == boot.ends && !boot.held);
    IdentityBinding observed;
    assert(storage.preparedBinding(observed) && observed.node == local.identity.node &&
           !memcmp(observed.publicKey, local.identity.publicKey, 32));
    PkiPeerSet pins;
    assert(storage.preparedPeers(pins) && pins.count() == expectedPins);
    assert(storage.activate(local.identity.node) == 0);
    assert(storage.state() == PkiStorage::State::Active);
  }

  void pause() { assert(!context.paused); context.paused = true; }
  void resume() { assert(context.paused); context.paused = false; }

  IdentityBinding binding() const { return local.identity; }

  KeyRef keyRef() {
    auto *identity = storage.runtimeIdentity();
    if (!identity) return {};
    assert(context.tryEnter());
    IdentityBinding b;
    KeyRef ref;
    assert(identity->binding(b, ref) && b.node == local.identity.node);
    context.leave();
    return ref;
  }

  void enableLegacyHistory() {
    const IdentityBinding expected = binding();
    pause();
    assert(storage.enableLongStorage(expected) == 0);
    resume();
    assert(storage.extendedStorage() && storage.appendRemaining() == PkiAppendHistory::EventLimit);
  }

  bool reserve(uint32_t id, uint32_t &extra) {
    const IdentityBinding b = binding();
    const KeyRef ref = keyRef();
    pause();
    const bool result = storage.reserve(ref, b.publicKey, b.node, id, extra);
    resume();
    return result;
  }

  bool commit(unsigned peer, uint32_t id) {
    assert(peer < 6);
    pause();
    const bool result = storage.commitAuthenticated(peers[peer].identity.node,
                       peers[peer].identity.publicKey, id);
    resume();
    return result;
  }

  int add(unsigned peer) {
    assert(peer < 6);
    PkiPeerPin pin;
    pin.node = peers[peer].identity.node;
    memcpy(pin.publicKey, peers[peer].identity.publicKey, 32);
    const auto b = binding();
    pause();
    const int result = storage.addTrustedPeer(b, pin);
    resume();
    return result;
  }

  bool revoke(unsigned peer) {
    assert(peer < 6);
    pause();
    const bool result = storage.revokeTrustedPeer(peers[peer].identity.node,
                                                   peers[peer].identity.publicKey);
    resume();
    return result;
  }

  int enableBounded() {
    const auto b = binding();
    pause();
    const int result = storage.enableBoundedHistory(b);
    resume();
    return result;
  }

  Durable snapshot() const { return Durable{settings.values, nor.bytes}; }
};

void fullStatus(Rig &rig) {
  PkiLedger::Status status{};
  assert(rig.storage.ledgerStatus(status));
  assert(status.format == 3 && status.sent > 0 && status.received >= 5 &&
         status.stored == 5 && status.active == 5);
  assert(rig.storage.extendedStorage() && rig.storage.appendRemaining() == 0);
}

Durable buildExhaustedLegacy() {
  Rig rig;
  rig.provisionLegacy();
  rig.prepareActive();
  rig.enableLegacyHistory();

  assert(rig.add(4) == 0); // Fifth logical contact is represented by an append.
  uint32_t extra = 0;
  assert(rig.reserve(LegacyTxAppend, extra) && extra != 0);
  assert(rig.commit(0, LegacyRxAppend));
  assert(rig.storage.active(rig.peers[4].identity.node, rig.peers[4].identity.publicKey));

  uint32_t id = FillIdBase;
  while (rig.storage.appendRemaining()) {
    const uint32_t before = rig.storage.appendRemaining();
    uint32_t nonce = 0;
    assert(rig.reserve(id++, nonce) && nonce != 0);
    assert(rig.storage.appendRemaining() == before - 1);
  }
  fullStatus(rig);
  assert(!rig.settings.values.count("mtpki/nonces0") &&
         !rig.settings.values.count("mtpki/nonces1"));
  assert(!memcmp(rig.nor.bytes.data() + SelectorOffset, "CMIT", 4));
  return rig.snapshot();
}

void testFullLegacyFailClosedAndActivation(const Durable &exhausted) {
  Rig rig(exhausted);
  assert(rig.storage.prepare() == 0);
  assert(!rig.storage.boundedHistory()); // Prepared has no app authority.
  uint32_t extra = 0xfeedbeefu;
  assert(!rig.reserve(0x70000001u, extra) && extra == 0xfeedbeefu);
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, 0x50000001u) == ReplayCheck::NotReady);
  assert(!rig.commit(0, 0x50000001u));
  assert(rig.storage.activate(rig.local.identity.node) == 0);
  fullStatus(rig);
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, 0x50000001u) == ReplayCheck::Full);
  const unsigned programs = rig.nor.programs;
  const unsigned writes = rig.settings.writes;
  const unsigned entropy = rig.entropy.calls;
  assert(!rig.reserve(LegacyTxBase, extra) && extra == 0xfeedbeefu);
  assert(!rig.reserve(LegacyTxAppend, extra) && extra == 0xfeedbeefu);
  assert(!rig.reserve(0x70000001u, extra) && extra == 0xfeedbeefu);
  assert(!rig.commit(0, 0x50000001u));
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, LegacyRxBase) == ReplayCheck::Seen);
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, LegacyRxAppend) == ReplayCheck::Seen);
  assert(rig.nor.programs == programs && rig.settings.writes == writes &&
         rig.entropy.calls == entropy);

  const Values oldSettings = rig.settings.values;
  const Bytes oldFlash = rig.nor.bytes;
  assert(rig.enableBounded() == 0);
  assert(rig.storage.boundedHistory() && rig.storage.nonceReservedThrough() == 0);
  assert(rig.storage.appendRemaining() == 0);
  assert(rig.settings.values.size() == oldSettings.size() + 2);
  for (const auto &entry : oldSettings)
    assert(rig.settings.values.at(entry.first) == entry.second);
  assert(rig.settings.values.at("mtpki/nonces0").size() == PkiNonceCounter::RecordBytes);
  assert(rig.settings.values.at("mtpki/nonces1").size() == PkiNonceCounter::RecordBytes);
  assert(rig.nor.bytes.size() == oldFlash.size());
  unsigned differences = 0;
  for (size_t i = 0; i < oldFlash.size(); ++i) {
    if (oldFlash[i] != rig.nor.bytes[i]) {
      ++differences;
      assert(i == SelectorOffset && uint8_t(oldFlash[i] ^ rig.nor.bytes[i]) == 1);
    }
  }
  assert(differences == 1 && !memcmp(rig.nor.bytes.data() + SelectorOffset, "BMIT", 4));
  assert(rig.nor.selectorAttempts == 1 && rig.nor.selectorPrograms == 1 && rig.nor.eraseCalls == 0);

  extra = 0xfeedbeefu;
  assert(!rig.reserve(0, extra) && extra == 0xfeedbeefu);
  assert(!rig.reserve(LegacyTxBase, extra) && extra == 0xfeedbeefu);
  assert(!rig.reserve(LegacyTxAppend, extra) && extra == 0xfeedbeefu);
  assert(rig.reserve(0x70000001u, extra) && extra == 1);
  assert(rig.reserve(0x70000002u, extra) && extra == 2);
  assert(rig.storage.nonceReservedThrough() == 256);
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, LegacyRxBase) == ReplayCheck::Seen);
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, LegacyRxAppend) == ReplayCheck::Seen);
  assert(rig.storage.seen(rig.peers[0].identity.node,
         rig.peers[0].identity.publicKey, 0x50000001u) == ReplayCheck::Fresh);

  const unsigned beforeFullPeerOps = rig.nor.programs;
  assert(rig.add(5) == -ENOSPC && !rig.revoke(0));
  assert(rig.nor.programs == beforeFullPeerOps);
  for (unsigned i = 0; i < 5; ++i)
    assert(rig.storage.active(rig.peers[i].identity.node, rig.peers[i].identity.publicKey));
}

void testBoundedReplayWindowAndRestart(const Durable &exhausted) {
  Durable selected;
  {
    Rig rig(exhausted);
    rig.prepareActive(5);
    assert(rig.enableBounded() == 0);
    uint32_t nonce = 0;
    assert(rig.reserve(0x70000011u, nonce) && nonce == 1);
    assert(rig.reserve(0x70000012u, nonce) && nonce == 2);
    for (unsigned i = 0; i < 5; ++i) {
      const uint32_t id = 0x51000000u + i;
      assert(rig.storage.seen(rig.peers[i].identity.node,
             rig.peers[i].identity.publicKey, id) == ReplayCheck::Fresh);
      assert(rig.commit(i, id));
      assert(rig.storage.seen(rig.peers[i].identity.node,
             rig.peers[i].identity.publicKey, id) == ReplayCheck::Seen);
    }
    // The fifth known peer evicts only a RAM replay window. It does not remove
    // any of the five logical PKI contacts or the old exact legacy fence.
    assert(rig.storage.active(rig.peers[0].identity.node, rig.peers[0].identity.publicKey));
    assert(rig.storage.active(rig.peers[4].identity.node, rig.peers[4].identity.publicKey));
    assert(rig.storage.seen(rig.peers[0].identity.node,
           rig.peers[0].identity.publicKey, 0x51000000u) == ReplayCheck::Fresh);
    assert(rig.storage.seen(rig.peers[0].identity.node,
           rig.peers[0].identity.publicKey, LegacyRxAppend) == ReplayCheck::Seen);
    assert(rig.storage.seen(rig.peers[0].identity.node,
           rig.peers[5].identity.publicKey, 0x52000000u) == ReplayCheck::WrongKey);
    assert(rig.storage.seen(rig.peers[5].identity.node,
           rig.peers[5].identity.publicKey, 0x52000000u) == ReplayCheck::UnknownPeer);
    selected = rig.snapshot();
  }
  {
    Rig reboot(selected);
    assert(reboot.storage.prepare() == 0);
    assert(!reboot.storage.boundedHistory());
    assert(reboot.storage.activate(reboot.local.identity.node) == 0);
    assert(reboot.storage.boundedHistory());
    assert(reboot.storage.nonceReservedThrough() == 256); // All old block IDs burned.
    assert(reboot.storage.seen(reboot.peers[0].identity.node,
           reboot.peers[0].identity.publicKey, LegacyRxBase) == ReplayCheck::Seen);
    assert(reboot.storage.seen(reboot.peers[0].identity.node,
           reboot.peers[0].identity.publicKey, LegacyRxAppend) == ReplayCheck::Seen);
    assert(reboot.storage.seen(reboot.peers[0].identity.node,
           reboot.peers[0].identity.publicKey, 0x51000000u) == ReplayCheck::Fresh);
    assert(reboot.commit(0, 0x51000000u));
    assert(reboot.storage.seen(reboot.peers[0].identity.node,
           reboot.peers[0].identity.publicKey, 0x51000000u) == ReplayCheck::Seen);
    uint32_t extra = 0;
    assert(reboot.reserve(0x70000003u, extra) && extra == 257);
    assert(reboot.storage.nonceReservedThrough() == 512);
    for (unsigned i = 0; i < 5; ++i)
      assert(reboot.storage.active(reboot.peers[i].identity.node,
                                   reboot.peers[i].identity.publicKey));
  }
}

struct CounterSettingsStore final : PkiNonceCounterStore {
  Settings &settings;
  explicit CounterSettingsStore(Settings &s) : settings(s) {}
  int read(unsigned slot, uint8_t *out, size_t capacity) override {
    return slot < 2 ? settings.read(slot ? "mtpki/nonces1" : "mtpki/nonces0",
                                    out, capacity) : -EINVAL;
  }
  int write(unsigned slot, const uint8_t *data, size_t size) override {
    return slot < 2 ? settings.write(slot ? "mtpki/nonces1" : "mtpki/nonces0",
                                     data, size) : -EINVAL;
  }
};

void injectGenesisCounters(Durable &durable) {
  Rig rig(durable);
  rig.prepareActive(5);
  PkiLedger::Status status{};
  assert(rig.storage.ledgerStatus(status) && status.format == 3 && status.generation);
  PkiNonceCounterBinding binding{};
  binding.node = rig.local.identity.node;
  memcpy(binding.publicKey, rig.local.identity.publicKey, 32);
  binding.legacyGeneration = status.generation;
  const uint8_t *root = rig.nor.bytes.data();
  binding.legacyCrc = uint32_t(root[4088]) | (uint32_t(root[4089]) << 8) |
                      (uint32_t(root[4090]) << 16) | (uint32_t(root[4091]) << 24);
  CounterSettingsStore counterStore(rig.settings);
  PkiNonceCounter counter(counterStore);
  rig.pause();
  assert(rig.context.depth == 0);
  rig.context.depth = 1; // Model the actual storage owner lock for staged-record injection.
  assert(counter.initialize(binding, true) == 0);
  rig.context.depth = 0;
  rig.resume();
  assert(!memcmp(rig.nor.bytes.data() + SelectorOffset, "CMIT", 4));
  durable = rig.snapshot();
}

void testUnexhaustedStagedCounterRefusal(const Durable &exhausted) {
  Durable partial = exhausted;
  const size_t lastCell = PkiAppendHistory::RootBytes +
      size_t(PkiAppendHistory::EventLimit - 1) * PkiAppendHistory::CellBytes;
  for (size_t i = 0; i < PkiAppendHistory::CellBytes; ++i)
    partial.flash[lastCell + i] = 0xff;
  for (unsigned corrupt = 0; corrupt < 2; ++corrupt) {
    Durable staged = partial;
    injectGenesisCounters(staged);
    if (corrupt) staged.settings["mtpki/nonces0"][0] ^= 0x40;
    Rig rig(staged);
    const unsigned writes = rig.settings.writes;
    assert(rig.storage.prepare() < 0);
    assert(rig.storage.state() == PkiStorage::State::Partial);
    assert(!rig.storage.boundedHistory());
    assert(rig.nor.selectorAttempts == 0 && rig.nor.selectorPrograms == 0 &&
           rig.nor.programs == 0 && rig.settings.writes == writes);
    const Durable after = rig.snapshot();
    assert(after.settings == staged.settings && after.flash == staged.flash);
  }
}

void testStagedAndDamagedCounterBootRefusal(const Durable &exhausted) {
  Durable staged = exhausted;
  injectGenesisCounters(staged);
  {
    Rig rig(staged);
    assert(rig.storage.prepare() < 0);
    assert(rig.storage.state() == PkiStorage::State::Partial);
    assert(rig.storage.activate(rig.local.identity.node) < 0);
    assert(!rig.storage.boundedHistory());
    uint32_t extra = 0x12345678;
    assert(!rig.reserve(0x70000001u, extra) && extra == 0x12345678);
  }

  Durable selected = exhausted;
  {
    Rig rig(selected);
    rig.prepareActive(5);
    assert(rig.enableBounded() == 0);
    selected = rig.snapshot();
  }
  for (unsigned caseNo = 0; caseNo < 3; ++caseNo) {
    Durable damaged = selected;
    if (caseNo == 0) damaged.settings.erase("mtpki/nonces0");
    if (caseNo == 1) damaged.settings.erase("mtpki/nonces1");
    if (caseNo == 2) damaged.settings["mtpki/nonces0"][0] ^= 0x40;
    Rig rig(damaged);
    assert(rig.storage.prepare() < 0);
    assert(rig.storage.state() == PkiStorage::State::Frozen);
    assert(rig.storage.activate(rig.local.identity.node) < 0);
    uint32_t extra = 0x12345678;
    assert(!rig.reserve(0x70000001u, extra) && extra == 0x12345678);
  }
}

void testSelectorAndCounterWritePowerCuts(const Durable &exhausted) {
  for (Nor::SelectorFault fault : {Nor::SelectorFault::Before,
                                    Nor::SelectorFault::After,
                                    Nor::SelectorFault::CorruptAfter}) {
    Rig first(exhausted);
    first.prepareActive(5);
    first.nor.selectorFault = fault;
    assert(first.enableBounded() < 0);
    assert(first.storage.state() == PkiStorage::State::Frozen);
    assert(first.nor.selectorAttempts == 1 && first.nor.eraseCalls == 0);
    uint32_t extra = 123;
    assert(!first.reserve(0x70000001u, extra) && extra == 123);
    const Durable afterCut = first.snapshot();
    if (fault == Nor::SelectorFault::Before) {
      assert(!memcmp(first.nor.bytes.data() + SelectorOffset, "CMIT", 4));
      assert(first.settings.values.count("mtpki/nonces0") &&
             first.settings.values.count("mtpki/nonces1"));
      Rig reboot(afterCut);
      assert(reboot.storage.prepare() < 0 &&
             reboot.storage.state() == PkiStorage::State::Partial);
      assert(reboot.storage.activate(reboot.local.identity.node) < 0);
    } else if (fault == Nor::SelectorFault::After) {
      assert(!memcmp(first.nor.bytes.data() + SelectorOffset, "BMIT", 4));
      Rig reboot(afterCut);
      assert(reboot.storage.prepare() == 0);
      assert(reboot.storage.activate(reboot.local.identity.node) == 0);
      assert(reboot.storage.boundedHistory() && reboot.storage.nonceReservedThrough() == 0);
      assert(reboot.reserve(0x70000001u, extra) && extra == 1);
    } else {
      assert(first.nor.bytes[SelectorOffset] == 'A');
      Rig reboot(afterCut);
      assert(reboot.storage.prepare() < 0);
      assert(reboot.storage.state() == PkiStorage::State::Frozen ||
             reboot.storage.state() == PkiStorage::State::Corrupt);
      assert(reboot.storage.activate(reboot.local.identity.node) < 0);
    }
  }

  for (unsigned failedWrite = 1; failedWrite <= 2; ++failedWrite) {
    for (Settings::Fault fault : {Settings::Fault::Before, Settings::Fault::After}) {
      Rig first(exhausted);
      first.prepareActive(5);
      first.settings.failWriteNumber = first.settings.writes + failedWrite;
      first.settings.fault = fault;
      assert(first.enableBounded() < 0);
      assert(first.storage.state() == PkiStorage::State::Frozen);
      assert(!memcmp(first.nor.bytes.data() + SelectorOffset, "CMIT", 4));
      const bool anyCounter = first.settings.values.count("mtpki/nonces0") ||
                              first.settings.values.count("mtpki/nonces1");
      const Durable afterCut = first.snapshot();
      Rig reboot(afterCut);
      if (anyCounter) {
        assert(reboot.storage.prepare() < 0 &&
               reboot.storage.state() == PkiStorage::State::Partial);
        assert(reboot.storage.activate(reboot.local.identity.node) < 0);
        uint32_t extra = 123;
        assert(!reboot.reserve(0x70000001u, extra) && extra == 123);
      } else {
        // No counter byte was committed. Legacy history remains intact and
        // fail-closed at full; only a new, explicit activation can retry.
        assert(reboot.storage.prepare() == 0);
        assert(reboot.storage.activate(reboot.local.identity.node) == 0);
        assert(!reboot.storage.boundedHistory());
        uint32_t extra = 123;
        assert(!reboot.reserve(0x70000001u, extra) && extra == 123);
        assert(reboot.enableBounded() == 0);
      }
    }
  }
}
} // namespace

int main() {
  const Durable exhausted = buildExhaustedLegacy();
  testFullLegacyFailClosedAndActivation(exhausted);
  testBoundedReplayWindowAndRestart(exhausted);
  testStagedAndDamagedCounterBootRefusal(exhausted);
  testUnexhaustedStagedCounterRefusal(exhausted);
  testSelectorAndCounterWritePowerCuts(exhausted);
  puts("PASS M148 synthetic storage lifecycle composition");
}
