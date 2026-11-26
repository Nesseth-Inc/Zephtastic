// SPDX-License-Identifier: GPL-3.0-only
// Inert target link proof. No constructor/startup call, identity, RNG or storage.
#include "PkiRuntime.h"
#include "PkiLedger.h"
#include "../Service.h"
namespace {
using namespace mt::pki;
using namespace mt::experimental;
void create(PkiCrypto &crypto,PkiBackend &backend,IdentityJournal &identity,
  NonceAuthority &nonces,PeerReplayAuthority &peers,SigningEntropy *signing) {
  PkiRuntime instance(crypto,backend,identity,nonces,peers,signing);
  instance.open(0,0);
}
// Qualified virtual calls retain actual implementations on ARM and Xtensa;
// member-function pointers alone may encode only a vtable offset.
void runtime(PkiRuntime &r,mt::Endpoint &ep,bench::MtCodec &codec,const bench::MtNodeRecord &record,
  const uint8_t *p,size_t n,const bench::RouteMetadata &m,PrivateEvent &event,bench::Frame &frame) {
  r.PkiRuntime::ready(0);r.PkiRuntime::publicIdentity(event.publicKey);
  r.PkiRuntime::bootstrap(codec,record,81,0,0,false,0,frame);
  r.PkiRuntime::submit(ep,0,0,0,p,n,0,false,nullptr,0,false,0);
  r.PkiRuntime::receive(ep,codec,p,n,0,0,false);r.PkiRuntime::proof(ep,p,n,m,0);
  r.PkiRuntime::event(0,p,n,event);r.PkiRuntime::sweep(ep);r.PkiRuntime::revoke(0);
  r.PkiRuntime::clearEvent();r.PkiRuntime::freeze();
}
void nonce(LedgerNonceAuthority &a,KeyRef ref,const uint8_t *pub,uint32_t &extra){a.LedgerNonceAuthority::reserve(ref,pub,0,0,extra);}
void replay(LedgerReplayAuthority &a,uint8_t *p){
  a.LedgerReplayAuthority::boundTo(p,0);a.LedgerReplayAuthority::lookup(0,p);
  a.LedgerReplayAuthority::active(0,p);a.LedgerReplayAuthority::revoked(0);a.LedgerReplayAuthority::seen(0,p,0);
  a.LedgerReplayAuthority::commitAuthenticated(0,p,0);a.LedgerReplayAuthority::revokeTrustedPeer(0,p);
}
struct Linkage {
  decltype(&create) createRuntime;decltype(&runtime) runtimeMethods;
  decltype(&PkiRuntime::open) open;decltype(&nonce) nonceMethod;decltype(&replay) replayMethods;
  decltype(&PkiLedger::load) load;decltype(&PkiLedger::initialize) initialize;
  decltype(&PkiLedger::addTrustedPeer) add;decltype(&PkiLedger::revokeTrustedPeer) revoke;
  decltype(&PkiLedger::close) close;decltype(&PkiSigner::ready) signerReady;
  decltype(&PkiSigner::sign) sign;decltype(&PkiSigner::bootstrap) bootstrap;
};
}
extern "C" const Linkage mt_pki_runtime_linkage={create,runtime,&PkiRuntime::open,nonce,replay,
  &PkiLedger::load,&PkiLedger::initialize,&PkiLedger::addTrustedPeer,&PkiLedger::revokeTrustedPeer,
  &PkiLedger::close,&PkiSigner::ready,&PkiSigner::sign,&PkiSigner::bootstrap};
// Ordered public size receipt: runtime,ledger,nonceadapter,replayadapter,signeddata,
// event,service. Explicitly allocated future providers would add these objects;
// the inert target proof does not allocate them in static RAM.
extern "C" const uint32_t mt_pki_runtime_sizes[]={sizeof(PkiRuntime),sizeof(PkiLedger),
  sizeof(LedgerNonceAuthority),sizeof(LedgerReplayAuthority),sizeof(SignedData),
  sizeof(PrivateEvent),sizeof(mt::Service)};
