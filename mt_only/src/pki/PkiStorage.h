// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "LegacyTailTools.h"
#include "PkiLedger.h"
#include "PkiPeerSet.h"
#include "PkiNonceCounter.h"
#include "PkiReplayWindow.h"
#include "PkiInplaceContacts.h"
#include "PkiFreshRoot.h"
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
#include "ExpandedContacts.h"
#endif
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
#include "LegacyTailProbe.h"
#include "FormerCodePreparation.h"
#endif

namespace mt::pki {
class PkiAppendHistory;
// Existing mounted store only. begin/end hold an immutable read snapshot;
// neither method may mount, initialize, write, delete or repair storage.
struct BootSettings:SettingsBackend {
  virtual ~BootSettings()=default;
  virtual int begin()=0;
  virtual void end()=0;
  int write(const char *,const uint8_t *,size_t) final{return -EACCES;}
};
struct PkiStorageContext:LedgerStorageGate {
  virtual bool tryEnter()=0; // Nonblocking acquisition of actual service mutex.
  virtual void leave()=0;
  virtual bool bootQuiescent()const=0;
};

// No constructor I/O, provisioning, trust creation or startup registration.
// Prepare exposes only verified public metadata; activation is an explicit
// node-bound choice. All runtime authority methods share the service lock.
class PkiStorage final:public NonceAuthority,public PeerReplayAuthority {
public:
  enum class State:uint8_t {New,Prepared,Active,Empty,Partial,Mismatch,Corrupt,Frozen,Closed,Busy};
private:
  BootSettings &boot_;SettingsBackend &runtimeBackend_;PkiStorageContext &context_;
#if !defined(CONFIG_MT_PKI_COMPACT_RUNTIME_ONLY)
  EntropySource &entropy_;
#endif
  PkiAppendHistory *append_=nullptr;bool extended_=false;
  IdentityJournal identity_;PkiLedger ledger_;MonocypherIdentityDeriver deriver_;
  mutable bool busy_=false;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  bool formerCodeAttempted_=false;
#endif
  State state_=State::New;int error_=0;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  TailPreparationResult preparationResult_;int preparationError_=-ENOENT;bool preparationObserved_=false;
#endif
  enum class Phase:uint8_t {None,Boot,Runtime};Phase phase_=Phase::None;
  class Backend final:public SettingsBackend {
    PkiStorage &owner_;
  public:
    explicit Backend(PkiStorage &o):owner_(o){}
    int read(const char *,uint8_t *,size_t)override;
    int write(const char *,const uint8_t *,size_t)override;
  } backend_;
  PkiInplaceContacts contacts_;
  class CounterStore final:public PkiNonceCounterStore {
    PkiStorage &owner_;
  public:
    explicit CounterStore(PkiStorage &owner):owner_(owner){}
    int read(unsigned,uint8_t *,size_t)override;
    int write(unsigned,const uint8_t *,size_t)override;
  } counterStore_;
  PkiNonceCounter counter_;
  PkiContactStore freshContacts_;PkiFreshRoot freshRoot_{};bool freshDomain_=false;
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  ExpandedContacts *expanded_=nullptr;
#endif
  bool freshReady()const{
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
    if(expanded_&&expanded_->state()!=ExpandedContacts::State::Legacy)return expanded_->ready();
#endif
    return freshContacts_.ready();
  }
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bool imported_=false;uint8_t importPermutation_[32]{};
#endif
  PkiReplayWindow window_;
  bool bounded_=false;uint8_t nextWindow_=0;
  LedgerNonceAuthority nonce_;LedgerReplayAuthority replay_;
  class Lease {
    const PkiStorage &owner_;bool held_=false;
  public:
    explicit Lease(const PkiStorage &);
    ~Lease();explicit operator bool()const{return held_;}
  };
  int fail(State,int);
  // prepare() admits only a verified fresh selector on the fresh-only board.
  // Mixed-layout builds must still dispatch on the selector discovered at boot.
#if defined(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
  int requireAbsent(unsigned end);
  static constexpr bool FreshOnly=true;
#else
  static constexpr bool FreshOnly=false;
#endif
  bool freshImplementation()const{return FreshOnly||freshDomain_;}
  bool usable()const;
  void observeFailure();
  bool counterBinding(PkiNonceCounterBinding &)const;
  bool legacyPins(PkiPeerPin *,size_t,size_t &)const;
  int loadContacts();
  bool contactPin(uint32_t,uint8_t *)const;
  ReplayCheck boundedLegacy(uint32_t,const uint8_t *,uint32_t)const;
  int windowIndex(uint32_t,const uint8_t *,uint32_t)const;
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  int legacyProof(PkiLedger::Status &,uint32_t &crc)const; // Lease already held.
#endif
public:
  PkiStorage(BootSettings &,SettingsBackend &runtimeBackend,PkiStorageContext &,EntropySource &,PkiAppendHistory *append=nullptr);
#if defined(CONFIG_MT_XIAO_EXPANDED_CONTACTS)
  void attachExpanded(ExpandedContacts &contacts){if(state_==State::New)expanded_=&contacts;}
  int enableExpanded(const IdentityBinding &expected);
  bool expandedActive()const{return expanded_&&expanded_->ready();}
  bool preparedRuntimePeers(RuntimePinDirectory &out)const;
#endif
#if defined(CONFIG_MT_STOCK_IDENTITY_IMPORT)
  bool importedIdentity() const {
    // Startup holds the Service lock and has already verified the root. Never
    // permit an unclassified origin if the storage lease cannot be acquired.
    Lease hold(*this);
    return !hold || imported_;
  }
#endif
  PkiStorage(const PkiStorage &)=delete;PkiStorage &operator=(const PkiStorage &)=delete;
  ~PkiStorage()override; // Owner must serialize destruction with all consumers.
  State state()const;
  int lastError()const;
  int prepare(); // 0=Prepared; Empty -ENOENT; Partial -ENODATA; Mismatch -EXDEV.
  bool preparedBinding(IdentityBinding &out)const; // No usable private handle.
  bool preparedPeer(uint32_t &out)const; // Exactly one already-pinned active peer.
  bool preparedPeers(PkiPeerSet &out)const; // All 1..16 validated active pins.
  bool contactRevision(uint32_t &out)const; // Changes on retirement and re-enrollment.
  bool ledgerStatus(PkiLedger::Status &out)const; // RAM-only, no key/ID export.
#if MT_PKI_LEGACY_TAIL_TOOLS_ENABLED
  // Read-only diagnostic. Legacy authority only; no key or snapshot export.
  // Digest/possibleJournal metadata cannot establish permission to erase.
  int probeLegacyTail(AppendStore &,TailProbeResult &,uint32_t &generation,uint32_t &snapshotCrc)const;
  int prepareFormerCode(AppendStore &,HistoryPageEraser &,PreparationProgress &,
                        const IdentityBinding &,uint32_t generation,uint32_t crc,TailPreparationResult &);
  bool preparationReport(TailPreparationResult &,int &error,bool &observed)const;
  void preparationRestoreFailed(int error); // RAM-only uncertainty; never retries.
#endif
  bool extendedStorage()const;
  uint16_t appendRemaining()const;
  // Explicit local administration only. Requires an entirely blank dedicated
  // region, an immutable root, then a verified NVS selector written last.
  int enableLongStorage(const IdentityBinding &expected);
  // Existing exhausted history stays intact. Both counter slots must be absent
  // at entry; interrupted bootstrap is held, never retried or repaired.
  int enableBoundedHistory(const IdentityBinding &expected);
  int enableCompactContacts(const IdentityBinding &expected);
  bool compactContacts()const;
  uint32_t contactGeneration()const;
  bool boundedHistory()const;
  uint32_t nonceReservedThrough()const;
  int activate(uint32_t expectedNode); // Exact prepared node, RF off, explicit.
  // Null until Active. Borrowed for PkiRuntime construction, never reload/store.
  // The caller MUST hold the actual service mutex throughout every use of the
  // returned identity and the resulting PkiRuntime (including ready/status).
  // Otherwise a contended authority check could be mistaken for revocation.
  IdentityJournal *runtimeIdentity();
  bool close(); // Terminal, key clearing, no storage writes.
  // Failure leaves caller output unchanged; only a successful reservation publishes it.
  bool reserve(KeyRef,const uint8_t localPublic[32],uint32_t local,uint32_t packetId,uint32_t &extra)override;
  bool persistentIds()const override{return !bounded_;}
  bool persistentNoncePairs()const override{return bounded_;}
  // Explicit same-device administration; caller supplies exact current public
  // binding and authorized pin while the real service storage pause is held.
  // A successful append becomes visible to sessions on a later reboot only.
  int addTrustedPeer(const IdentityBinding &expected,const PkiPeerPin &pin);
  // Explicit local-owner key installation/replacement into an existing compact
  // contact authority. Caller owns transport authorization and quiescence.
  using BeforeOwnerPin=int (*)(uint32_t);
  int putOwnerPeer(const IdentityBinding &expected,const PkiPeerPin &pin,BeforeOwnerPin before=nullptr);
  bool boundTo(const uint8_t localPublic[32],uint32_t local)const override;
  bool lookup(uint32_t peer,uint8_t outPublic[32])const override;
  bool active(uint32_t peer,const uint8_t publicKey[32])const override;
  bool revoked(uint32_t peer)const override;
  ReplayCheck seen(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId)const override;
  bool commitAuthenticated(uint32_t peer,const uint8_t publicKey[32],uint32_t packetId)override;
  bool revokeTrustedPeer(uint32_t peer,const uint8_t publicKey[32])override;
};
}
