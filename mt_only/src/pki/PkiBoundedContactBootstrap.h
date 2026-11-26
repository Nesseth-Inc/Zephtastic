// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "PkiAppendHistory.h"
#include "PkiNonceCounter.h"
#include "PkiIdentity.h"

namespace mt::pki {

// Cold bounded-history composition. The reconstructed baseline, replayed
// history, and nonce allocator stay private until all three bindings validate.
// Caller must serialize prepare() against storage mutation and RX startup.
class PkiBoundedContactBootstrap {
public:
  enum class State:uint8_t {New,Ready,Failed,Closed};

  PkiBoundedContactBootstrap(AppendStore &,PkiNonceCounterStore &,IdentityJournal &);
  PkiBoundedContactBootstrap(const PkiBoundedContactBootstrap &)=delete;
  PkiBoundedContactBootstrap &operator=(const PkiBoundedContactBootstrap &)=delete;
  ~PkiBoundedContactBootstrap();

  // One attempt only. On any root, replay, identity, or nonce failure, the
  // staged objects are cleared and no peer/status result is published.
  int prepare();
  State state()const{return state_;}
  bool ready()const{return state_==State::Ready;}
  bool status(PkiLedger::Status &)const;
  bool activePeers(uint32_t *out,size_t capacity,size_t &count)const;
  bool lookup(uint32_t node,uint8_t publicKey[32])const;
  uint32_t nonceReservedThrough()const;

private:
  AppendStore &appendStore_;
  IdentityJournal &identity_;
  PkiLedger ledger_;
  PkiAppendHistory history_;
  PkiNonceCounter counter_;
  State state_=State::New;
  int error_=0;
  int fail(int);
};

}
