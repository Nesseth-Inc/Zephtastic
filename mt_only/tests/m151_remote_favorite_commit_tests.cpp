// SPDX-License-Identifier: GPL-3.0-only
// Synthetic in-memory storage and radio only; no device or RF activity.
#include "pki_runtime_fixture.h"
#include "../src/admin/RemoteFavoriteCommit.h"
#include <cerrno>
#include <cstdio>

using namespace mt;
using namespace mt::pki;

namespace {
constexpr uint32_t PeerNode = 0x23456789u;

struct NodeStore final : SettingsBackend {
  std::map<std::string, Bytes> rows;
  unsigned writes = 0;
  int fault = 0; // 1: fail before commit, 2: commit then report uncertainty.
  int read(const char *key, uint8_t *out, size_t cap) override {
    const auto it = rows.find(key);
    if (it == rows.end()) return -ENOENT;
    if (it->second.size() > cap) return -E2BIG;
    memcpy(out, it->second.data(), it->second.size());
    return int(it->second.size());
  }
  int write(const char *key, const uint8_t *p, size_t n) override {
    ++writes;
    if (fault == 1) return -EIO;
    rows[key] = Bytes(p, p + n);
    return fault == 2 ? -EIO : 0;
  }
};

Bytes ownerGet() {
  meshtastic_AdminMessage admin{};
  admin.which_payload_variant = meshtastic_AdminMessage_get_owner_request_tag;
  admin.get_owner_request = true;
  uint8_t encodedAdmin[233]{};
  auto as = pb_ostream_from_buffer(encodedAdmin, sizeof(encodedAdmin));
  assert(pb_encode(&as, meshtastic_AdminMessage_fields, &admin));
  meshtastic_Data data{};
  data.portnum = meshtastic_PortNum_ADMIN_APP;
  data.want_response = true;
  data.has_bitfield = true;
  data.bitfield = 2;
  data.payload.size = as.bytes_written;
  memcpy(data.payload.bytes, encodedAdmin, data.payload.size);
  uint8_t wire[227]{};
  auto ds = pb_ostream_from_buffer(wire, sizeof(wire));
  assert(pb_encode(&ds, meshtastic_Data_fields, &data));
  return Bytes(wire, wire + ds.bytes_written);
}

using PkiFixture = ::Fixture;
struct Fixture : PkiFixture {
  NodeStore nodeStore;
  NodeJournal journal;
  uint32_t now = 100;

  explicit Fixture(bool router = false) {
    // The shared PKI fixture defaults to TX-disabled; the production remote
    // mutation gate requires a healthy, armed normal-mode session.
    s.stop();
    Config config = s.config;
    config.setTxEnabled(true);
    if(router)assert(config.setDeviceRole(2,5));
    assert(s.apply(config) && s.start(false, now) && s.armEndpoint(true));
    assert(journal.load(nodeStore, s.nodes, local.binding.node) == 0);
    NodeCache::Entry entry{};
    entry.identity(PeerNode, 127, 1);
    assert(NodeCache::Entry::text(entry.bytes + NodeCache::Entry::Name,
                                  NodeCache::Entry::Name, "synthetic", 10));
    assert(NodeCache::Entry::text(entry.bytes + NodeCache::Entry::Short,
                                  NodeCache::Entry::Short, "syn", 4));
    assert(s.nodes.accept(entry, local.binding.node, -20, 1) >= 0);
  }

  int edit(bool favorite, bool blocked = false) {
    return admin::RemoteFavoriteCommit::edit(s, radio, journal, nodeStore,
                                              PeerNode, favorite, ++now, blocked);
  }
};

void localAndRemoteFavoriteEditsShareArmedCommitAndRestore() {
  Fixture f;
  const auto before = Bytes(f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)))->bytes,
                            f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)))->bytes + NodeCache::Entry::Size);
  const unsigned initialStops = f.radio.stops;
  const int localResult=NodeAdmin::edit(f.s,f.radio,f.journal,f.nodeStore,PeerNode,
                                        NodeCache::Edit::Favorite,++f.now);
  if(localResult!=0)fprintf(stderr,"local favorite result=%d ready=%d nodeReady=%d fault=%d rx=%d arm=%d health=%d gen=%u writes=%u\n",localResult,f.journal.ready(),f.s.nodes.ready(),f.s.faulted(),f.s.receiving(),f.s.endpointArmed(),f.radio.healthValue,f.journal.generation(),f.nodeStore.writes);
  assert(localResult==0);
  assert(f.nodeStore.writes==1&&f.journal.generation()==1&&
         f.s.receiving()&&f.s.endpointArmed()&&f.radio.healthValue==1&&
         f.radio.stops==initialStops+1&&f.radio.sends==0);
  assert(f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)))->favorite());

  // The same state through the remote-specific wrapper is idempotent and does
  // not take a second radio pause or write another NodeJournal generation.
  const unsigned favoriteStops=f.radio.stops;
  const int favoriteResult=f.edit(true);
  if (favoriteResult != 0) fprintf(stderr, "favorite result=%d ready=%d nodeReady=%d fault=%d rx=%d arm=%d normal=%d tx=%d cfgTX=%d health=%d gen=%u writes=%u\n", favoriteResult, f.journal.ready(), f.s.nodes.ready(), f.s.faulted(), f.s.receiving(), f.s.endpointArmed(), f.s.normalMode(), f.s.txAllowed(), f.s.config.txEnabled(), f.radio.healthValue, f.journal.generation(), f.nodeStore.writes);
  assert(favoriteResult == 0);
  assert(f.nodeStore.writes == 1 && f.journal.generation() == 1 &&f.radio.stops==favoriteStops&&
         f.s.receiving() && f.s.endpointArmed() && f.radio.healthValue == 1 &&
         f.radio.sends == 0);
  const auto *changed = f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)));
  assert(changed && changed->favorite());
  for (unsigned i = 0; i < NodeCache::Entry::Size; ++i) {
    const uint8_t expected = i == NodeCache::Entry::LocalFlags
        ? uint8_t(before[i] | NodeCache::Entry::Favorite) : before[i];
    assert(changed->bytes[i] == expected);
  }
  NodeCache reloaded{};
  NodeJournal check;
  assert(check.load(f.nodeStore, reloaded, f.local.binding.node) == 0 &&
         reloaded.get(unsigned(reloaded.find(PeerNode)))->favorite());

  assert(f.edit(false) == 0);
  assert(f.nodeStore.writes == 2 && f.journal.generation() == 2 &&
         f.s.receiving() && f.s.endpointArmed() && f.radio.sends == 0);
  assert(!f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)))->favorite());
  assert(check.load(f.nodeStore, reloaded, f.local.binding.node) == 0 &&
         !reloaded.get(unsigned(reloaded.find(PeerNode)))->favorite());
}

void refusalsDoNotPauseOrWrite() {
  {
    Fixture f;
    const unsigned stops = f.radio.stops;
    assert(f.edit(true, true) == -EBUSY);
    assert(f.nodeStore.writes == 0 && f.radio.stops == stops && f.s.receiving() && f.s.endpointArmed());
  }
  {
    Fixture f;
    f.radio.busyValue = true;
    const unsigned stops = f.radio.stops;
    assert(f.edit(true) == -EBUSY);
    assert(f.nodeStore.writes == 0 && f.radio.stops == stops && f.s.receiving() && f.s.endpointArmed());
  }
  {
    Fixture f;
    f.radio.healthValue = 0; // stale Service RX bookkeeping cannot authorize a write.
    const unsigned stops = f.radio.stops;
    assert(f.edit(true) == -EIO);
    assert(f.nodeStore.writes == 0 && f.radio.stops == stops && f.s.receiving() && f.s.endpointArmed());
  }
  {
    Fixture f;
    auto request = ownerGet();
    assert(f.s.submitPrivateAdminGet(0, f.peer.binding.node, 120, request.data(), request.size(),
                                     3, false, nullptr, 0, true, 1500) == 0);
    f.s.poll(2000, 71);
    assert(f.radio.sends == 1);
    f.radio.done = true;
    f.s.poll(2200, 71);
    assert(f.s.privateAdminQueriesPending());
    const unsigned stops = f.radio.stops;
    assert(f.edit(true) == -EBUSY);
    assert(f.nodeStore.writes == 0 && f.radio.stops == stops &&
           f.s.receiving() && f.s.endpointArmed() && f.s.privateAdminQueriesPending());
  }
  {
    Fixture f;
    f.radio.stopError = -ESHUTDOWN;
    assert(f.edit(true) == -EIO);
    assert(f.nodeStore.writes == 0 && f.s.faulted() && !f.s.receiving() &&
           !f.s.endpointArmed());
  }
}

void armedLocalEditPreservesRelayIntent() {
  Fixture f(true);
  assert(f.s.receiving()&&f.s.endpointArmed()&&f.s.relaying());
  assert(NodeAdmin::edit(f.s,f.radio,f.journal,f.nodeStore,PeerNode,
                         NodeCache::Edit::Favorite,++f.now)==0);
  assert(f.s.receiving()&&f.s.endpointArmed()&&f.s.relaying()&&
         f.radio.healthValue==1&&f.nodeStore.writes==1&&
         f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)))->favorite());
}

void removingLastPeerLeavesDormantRuntimeOff() {
  Fixture f;
  assert(f.s.receiving()&&f.s.endpointArmed()&&f.runtime.ready(f.local.binding.node));
  const uint32_t target=f.peer.binding.node;
  if(f.s.nodes.find(target)<0) {
    NodeCache::Entry entry{};entry.identity(target,127,1);
    assert(NodeCache::Entry::text(entry.bytes+NodeCache::Entry::Name,
                                  NodeCache::Entry::Name,"PKI peer",9));
    assert(NodeCache::Entry::text(entry.bytes+NodeCache::Entry::Short,
                                  NodeCache::Entry::Short,"PKI",4));
    entry.bytes[NodeCache::Entry::KeyLength]=32;
    memcpy(entry.bytes+NodeCache::Entry::Key,f.peer.binding.publicKey,32);
    assert(f.s.nodes.accept(entry,f.local.binding.node,-20,1)>=0);
  }
  const int result=NodeAdmin::edit(f.s,f.radio,f.journal,f.nodeStore,target,
                                   NodeCache::Edit::Remove,++f.now);
  assert(result==0&&f.nodeStore.writes==1&&f.journal.generation()==1&&
         f.s.nodes.find(target)<0&&!f.runtime.ready(f.local.binding.node)&&
         !f.s.receiving()&&!f.s.endpointArmed()&&!f.s.faulted()&&f.radio.healthValue==0);
}

void uncertainPersistenceFailsClosedAndRestoreFailureKeepsCommitMeaning() {
  for (int fault : {1, 2}) {
    Fixture f;
    f.nodeStore.fault = fault;
    assert(f.edit(true) == -EIO);
    assert(f.nodeStore.writes == 1 && !f.journal.ready() && !f.s.receiving() &&
           !f.s.endpointArmed() && f.radio.healthValue == 0);
  }
  {
    Fixture f;
    f.radio.startError = -EIO;
    assert(f.edit(true) == NodeAdmin::CommittedRxFault);
    assert(f.nodeStore.writes == 1 && f.journal.ready() &&
           f.s.nodes.get(unsigned(f.s.nodes.find(PeerNode)))->favorite() &&
           !f.s.receiving() && !f.s.endpointArmed() && f.s.faulted());
  }
}
} // namespace

int main() {
  localAndRemoteFavoriteEditsShareArmedCommitAndRestore();
  puts("PASS local and remote favorite edits share the armed RF-safe commit barrier");
  armedLocalEditPreservesRelayIntent();
  puts("PASS armed local edit preserves configured relay intent");
  removingLastPeerLeavesDormantRuntimeOff();
  puts("PASS final target-peer removal commits NodeDB and leaves no-authority runtime RF-off");
  refusalsDoNotPauseOrWrite();
  puts("PASS blocked, busy, pending GET, and radio-stop failures refuse without lost correlations");
  uncertainPersistenceFailsClosedAndRestoreFailureKeepsCommitMeaning();
  puts("PASS uncertain storage fails closed and verified commit/RX fault remains distinct");
}
