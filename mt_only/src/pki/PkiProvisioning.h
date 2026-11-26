// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "PkiStorage.h"

namespace mt::pki {
struct ProvisionPeer {uint32_t node=0;uint8_t publicKey[32]{};};
enum class ProvisionState:uint8_t {New,Staged,Committing,Committed,Rejected,Expired,Cancelled,Frozen,Busy};
enum class ProvisionStored:uint8_t {Unknown,Empty,Partial,Complete,Mismatch,Corrupt,Frozen};
struct ProvisionStatus {
  ProvisionState attempt=ProvisionState::New;
  ProvisionStored stored=ProvisionStored::Unknown;
  int error=0;
  // Populated only for a fully validated, same-domain, single-pin tuple.
  bool hasBinding=false,hasPeer=false;
  IdentityBinding local{};ProvisionPeer peer{};
};
struct ProvisioningContext {
  virtual ~ProvisioningContext()=default;
  virtual bool tryEnter()=0; // Actual service mutex, nonblocking/recursive.
  virtual void leave()=0;
  // Same authorized USB session, actual LoRa ready/off/not busy, TX disarmed,
  // no active private runtime, queues, SettingsEdit batch or reboot/bootloader
  // request. This predicate does not stop RF or grant provisioning permission.
  virtual bool quiescent()const=0;
  virtual uint64_t millis()const=0; // Trusted 64-bit monotonic uptime, never RNG.
  // Read-only current hardware-ID/reserved-ID/NodeDB collision policies.
  // The core independently validates canonical keys, CRC node binding and self.
  virtual bool candidateAllowed(const IdentityBinding &)const=0;
  virtual bool peerAllowed(const IdentityBinding &,const ProvisionPeer &)const=0;
};

// One object owns both the immutable read snapshots and writes for the same
// already-mounted store/namespace. This shape prevents accidentally checking
// one backend for Empty and committing to another.
struct ProvisioningSettings:BootSettings {
  virtual int persist(const char *,const uint8_t *,size_t)=0;
};

// Explicit fresh provisioning only. No constructor I/O, transport handler,
// private export/provider, auto-pairing, recovery, overwrite or activation.
// A single attempt per instance. The owner services tick() and cancels on USB
// disconnect; elapsed time is checked when called, not by a background timer.
class PkiProvisioning {
public:
  using State=ProvisionState;
  static constexpr uint64_t StageLifetimeMs=120000;
private:
  ProvisioningSettings &settings_;ProvisioningContext &context_;EntropySource &entropy_;
  MonocypherIdentityDeriver deriver_;
  uint8_t scalar_[32]{};IdentityBinding staged_{};uint64_t started_=0;
  mutable bool busy_=false;bool attemptedWrite_=false,wroteStep_=false;
  State state_=State::New;int error_=0;unsigned step_=0;
  class Lease {
    const PkiProvisioning &owner_;bool held_=false;
  public:
    explicit Lease(const PkiProvisioning &);~Lease();
    explicit operator bool()const{return held_;}
  };
  class Backend final:public SettingsBackend {
    PkiProvisioning &owner_;
  public:
    explicit Backend(PkiProvisioning &owner):owner_(owner){}
    int read(const char *,uint8_t *,size_t)override;
    int write(const char *,const uint8_t *,size_t)override;
  } backend_;
  void wipe();
  int fail(State,int);
  bool expired()const;
  int gate()const;
  int endFailure(int);
  int load(IdentityJournal &,PkiLedger &);
  bool exact(const IdentityBinding &)const;
public:
  PkiProvisioning(ProvisioningSettings &,ProvisioningContext &,EntropySource &);
  PkiProvisioning(const PkiProvisioning &)=delete;
  PkiProvisioning &operator=(const PkiProvisioning &)=delete;
  ~PkiProvisioning(); // Owner serializes destruction with every consumer.
  State state()const;
  int lastError()const;
  // Only New consumes entropy: exactly one32-byte draw, no redraw on rejection.
  // Staged returns the SAME public candidate if still permitted/unexpired.
  // Any consumed stage/commit validation failure is terminal and wipes RAM.
  // Lease contention/reentry returns -EBUSY without mutating the ongoing owner.
  // Output is unchanged on failure. No usable private handle is returned.
  int stage(IdentityBinding &publicOut);
  // Exact expected public fields+bytes and one explicitly authorized peer pin.
  // Order: ledger0(domain) -> ledger1(pin) -> identity0(completion), each with
  // exact no-repair readback. A write attempt/error is terminal uncertainty;
  // no retry, delete, rollback, reboot, history reset or activation is performed.
  int commit(const IdentityBinding &expected,const ProvisionPeer &explicitPin);
  int tick(); // Expired/lost-session candidates are cleared; performs no I/O.
  bool cancel(); // Only New/Staged; terminal and no persistent writes.
  // Read-only observation, never permission to retry/activate/recover. A later
  // explicit reboot may observe Complete despite an earlier uncertain return.
  // inspect/tick may expire staged RAM. Rejected lease/gate leaves out unchanged;
  // read failures publish only a typed state/error, never unverified metadata.
  int inspect(ProvisionStatus &out);
};
}
