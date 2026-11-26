// SPDX-License-Identifier: GPL-3.0-only
// Opt-in link retention only. No caller, startup initializer, key, RNG or RF I/O.
#include "PsaPkiBackend.h"
#include "PkiSession.h"
#include "PkiAck.h"
#include "PkiIdentity.h"
namespace {
using namespace mt::pki;
using namespace mt::experimental;
void retainBackend(){PsaPkiBackend backend;}
void retainIdentity(){IdentityJournal journal;MonocypherIdentityDeriver deriver;}
bool retainDerivation(const uint8_t *scalar,uint8_t *publicKey){
  MonocypherIdentityDeriver deriver;
  return deriver.derivePublic(scalar,publicKey);
}
void retainCandidate(const IdentityBinding &binding,const uint8_t *scalar){
  IdentityCandidate candidate(binding,scalar);
  IdentityCandidate moved(static_cast<IdentityCandidate &&>(candidate));
}
struct Linkage {
  decltype(&retainBackend) backend;
  decltype(&PsaPkiBackend::begin) begin;
  decltype(&PsaPkiBackend::close) closeBackend;
  decltype(&PkiSession::open) open;
  decltype(&PkiSession::close) close;
  decltype(&PkiSession::send) send;
  decltype(&PkiSession::retry) retry;
  decltype(&PkiSession::retire) retire;
  decltype(&PkiSession::receive) receive;
  decltype(&PkiAck::inspect) inspect;
  decltype(&PkiAck::attach) attach;
  decltype(&PkiAck::verify) verify;
  decltype(&retainIdentity) identity;
  decltype(&retainDerivation) derivation;
  decltype(&retainCandidate) candidate;
  decltype(&IdentityJournal::load) loadIdentity;
  decltype(&IdentityJournal::store) storeIdentity;
  decltype(&IdentityJournal::binding) identityBinding;
  decltype(&IdentityJournal::active) activeIdentity;
  decltype(&IdentityJournal::withPrivate) withPrivate;
  decltype(&IdentityJournal::close) closeIdentity;
  decltype(&MonocypherIdentityDeriver::derivePublic) derivePublic;
  size_t sessionBytes,backendBytes,identityBytes,candidateBytes;
};
}
extern "C" const Linkage mt_pki_linkage={
  retainBackend,&PsaPkiBackend::begin,&PsaPkiBackend::close,
  &PkiSession::open,&PkiSession::close,&PkiSession::send,&PkiSession::retry,
  &PkiSession::retire,&PkiSession::receive,&PkiAck::inspect,&PkiAck::attach,&PkiAck::verify,
  retainIdentity,retainDerivation,retainCandidate,&IdentityJournal::load,&IdentityJournal::store,
  &IdentityJournal::binding,&IdentityJournal::active,&IdentityJournal::withPrivate,
  &IdentityJournal::close,&MonocypherIdentityDeriver::derivePublic,
  sizeof(PkiSession),sizeof(PsaPkiBackend),sizeof(IdentityJournal),sizeof(IdentityCandidate)
};
