// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiIdentity.h"
#include "PkiSession.h"
#include "PkiReplayAuthority.h"
#include "AppendStore.h"

namespace mt::pki {
struct EntropySource {
  virtual ~EntropySource()=default;
  // Trusted reviewed CSPRNG with a proven ready entropy source; return false on
  // any unavailable/partial/error state. No clock, hardware-ID, sys_rand32_get
  // or weak PRNG fallback. No concrete target adapter is supplied here.
  virtual bool fill(uint8_t *out,size_t size)=0;
};
struct LedgerStorageGate {
  virtual ~LedgerStorageGate()=default;
  // Permission held under service serialization for the complete commit. A
  // future service must verify radio stop/health; a boolean does not stop RF.
  virtual bool quiescent()const=0;
};

// One immutable local key/node domain. This journal never generates keys,
// advertises trust, evicts IDs, forgets peers, migrates domains or resets history.
// Revocation appends a permanent tombstone, retaining the pin and all history.
// No firmware/default backend/provisioning route is wired to this source.
class PkiLedger {
public:
  static constexpr size_t SendLimit=144,PeerLimit=4,ReceiveLimit=72;
  static constexpr size_t LegacyRecordSize=1752;
  static constexpr size_t HeaderSize=52,PeerSize=40+ReceiveLimit*4;
  static constexpr size_t PeerOffset=HeaderSize+SendLimit*4;
  static constexpr size_t RecordSize=PeerOffset+PeerLimit*PeerSize+4;
  enum class State:uint8_t {Unloaded,Empty,Ready,CorruptFrozen,IoFrozen,Closed};
  struct Status {uint32_t generation=0;uint16_t sent=0,received=0,stored=0;uint8_t format=0,active=0;};
private:
  uint8_t record_[RecordSize]{};
  State state_=State::Unloaded;
  int8_t slot_=-1;
  bool busy_=false;uint8_t wireVersion_=0;
  bool anchored_=false;AppendAnchor anchor_{};
  int freeze(State,int);
  int commit(SettingsBackend &);
  int peerIndex(uint32_t)const;
public:
  PkiLedger()=default;
  PkiLedger(const PkiLedger &)=delete;
  PkiLedger &operator=(const PkiLedger &)=delete;
  ~PkiLedger();
  State state()const{return state_;}
  bool ready()const{return state_==State::Ready&&!busy_;}
  // Load once. Every present slot must validate; no recovery from one bad slot.
  int load(SettingsBackend &);
  // Internal migration seam. This is a canonical public pin/packet-ID snapshot,
  // never an identity/private-key export. A selector makes this baseline
  // permanently read-only; only its validated append history may authorize I/O.
  bool snapshotRead(size_t offset,uint8_t *out,size_t size)const;
  // Strictly validate a canonical v2 snapshot in RAM. This is a decoder only;
  // it neither constructs a ledger nor accesses settings storage.
  static bool validateSnapshot(const uint8_t *snapshot,size_t size);
  static bool matchesAppendSelector(const uint8_t *,size_t,const AppendAnchor &);
  // Recreate only the in-RAM append baseline decoded from an immutable root.
  // Callers must immediately validate/replay the matching root with
  // PkiAppendHistory::open before using this ledger as effective peer state.
  int loadValidatedAppendBaseline(const uint8_t *snapshot,size_t size,
                                  const AppendAnchor &,uint32_t localNode,
                                  const uint8_t localPublic[32]);
  bool appendAnchor(AppendAnchor &out)const;
  int installAppendAnchor(SettingsBackend &,const AppendAnchor &,bool quiescent);
  // Explicit future authorized provisioning only, never an automatic response
  // to missing/corrupt storage. First domain immutable thereafter, even Empty
  // reload/erase/reimport cannot be requested through this API.
  int initialize(SettingsBackend &,const uint8_t localPublic[32],uint32_t local,bool quiescent);
  // Explicit previously-authorized trust pin, not unverified received NodeInfo.
  // Existing node/key or key/node collisions are refused; no replacement API.
  int addTrustedPeer(SettingsBackend &,uint32_t peer,const uint8_t publicKey[32],bool quiescent);
  int revokeTrustedPeer(SettingsBackend &,uint32_t peer,const uint8_t publicKey[32],bool quiescent);
  bool boundTo(const uint8_t localPublic[32],uint32_t local)const;
  bool lookup(uint32_t peer,uint8_t outPublic[32])const;
  bool active(uint32_t peer,const uint8_t publicKey[32])const;
  bool revoked(uint32_t peer)const;
  bool singleActivePeer(uint32_t &peer)const; // Exactly one active pin; tombstones do not count.
  // Fresh provisioning verification is stricter than runtime startup: no
  // historical tombstones or additional stored pins may exist.
  bool singleStoredActivePeer(uint32_t &peer)const;
  // Snapshot of already trusted, non-revoked nodes, in persistent order.
  // No trust mutation; all outputs unchanged on failure, zero is a valid count.
  bool activePeers(uint32_t *out,size_t capacity,size_t &count)const;
  bool status(Status &out)const; // Verified selected wire format/counters only.
  bool sent(uint32_t packetId)const;
  ReplayCheck seen(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId)const;
  int reserve(SettingsBackend &,uint32_t packetId,bool quiescent);
  int commitAuthenticated(SettingsBackend &,uint32_t peer,const uint8_t publicKey[32],uint32_t packetId,bool quiescent);
  bool close(); // Clears RAM, does not erase history or permit reinitialization.
};
static_assert(PkiLedger::RecordSize==1944,"Ledger v2 record budget");
static_assert(PkiLedger::RecordSize<4096,"Record fits current NVS sector");

class LedgerNonceAuthority final:public NonceAuthority {
  IdentityJournal &identity_;PkiLedger &ledger_;SettingsBackend &backend_;
  EntropySource &entropy_;LedgerStorageGate &gate_;bool busy_=false;
public:
  LedgerNonceAuthority(IdentityJournal &i,PkiLedger &l,SettingsBackend &b,EntropySource &e,LedgerStorageGate &g):
    identity_(i),ledger_(l),backend_(b),entropy_(e),gate_(g){}
  LedgerNonceAuthority(const LedgerNonceAuthority &)=delete;
  LedgerNonceAuthority &operator=(const LedgerNonceAuthority &)=delete;
  bool reserve(KeyRef,const uint8_t localPublic[32],uint32_t local,uint32_t packetId,uint32_t &extra) override;
  bool persistentIds()const override{return true;}
};
class LedgerReplayAuthority final:public PeerReplayAuthority {
  IdentityJournal &identity_;PkiLedger &ledger_;SettingsBackend &backend_;LedgerStorageGate &gate_;bool busy_=false;
  bool identityBound()const;
public:
  LedgerReplayAuthority(IdentityJournal &i,PkiLedger &l,SettingsBackend &b,LedgerStorageGate &g):
    identity_(i),ledger_(l),backend_(b),gate_(g){}
  LedgerReplayAuthority(const LedgerReplayAuthority &)=delete;
  LedgerReplayAuthority &operator=(const LedgerReplayAuthority &)=delete;
  bool boundTo(const uint8_t localPublic[32],uint32_t local)const override;
  bool lookup(uint32_t peer,uint8_t outPublic[32])const override;
  bool active(uint32_t peer,const uint8_t publicKey[32])const override;
  bool revoked(uint32_t peer)const override;
  ReplayCheck seen(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId)const override;
  bool commitAuthenticated(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId) override;
  bool revokeTrustedPeer(uint32_t peer,const uint8_t publicKey[32]) override;
};
}
