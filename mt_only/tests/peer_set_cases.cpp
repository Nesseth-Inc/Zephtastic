// SPDX-License-Identifier: GPL-3.0-only
// peer_set_cases.cpp — deterministic PkiPeerSet assertion suite.
// Synthetic X25519 fixtures only (clamped scalars, derived keys); no real
// identity/key material. Standalone main(); root links the pinned crypto.
#include "PkiPeerSet.h"
#include "monocypher.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace mt::pki;

namespace {
// Valid identity constants — must match PkiPeerSet.h; adjust if different.
constexpr unsigned kVersion = 1, kAlgo = 1;
// Distinct clamped synthetic scalars: s[0]&7==0 and (s[31]&0xc0)==0x40.
const unsigned char SL[32] = {0,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x08,0x19,0x2a,0x3b,0x4c,0x5d,0x6e,0x7f,0,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x08,0x19,0x2a,0x3b,0x4c,0x5d,0x6e,0x41};
const unsigned char S1[32] = {0,1,2,3,4,5,6,7,8,9,0xa,0xb,0xc,0xd,0xe,0xf,0,1,2,3,4,5,6,7,8,9,0xa,0xb,0xc,0xd,0xe,0x40};
const unsigned char S2[32] = {8,0x10,0x20,0x30,0x40,0x50,0x60,0x70,8,0x18,0x28,0x38,0x48,0x58,0x68,0x78,8,0x10,0x20,0x30,0x40,0x50,0x60,0x70,8,0x18,0x28,0x38,0x48,0x58,0x68,0x48};
const unsigned char S3[32] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x50};
const unsigned char S4[32] = {0x18,0x21,0x32,0x43,0x54,0x65,0x76,0x87,0x28,0x39,0x4a,0x5b,0x6c,0x7d,0x8e,0x9f,0x18,0x21,0x32,0x43,0x54,0x65,0x76,0x87,0x28,0x39,0x4a,0x5b,0x6c,0x7d,0x8e,0x60};
int fails = 0;
#define CK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
void pub(const unsigned char* s, unsigned char* p) { crypto_x25519_public_key(p, s); }
unsigned nodeId(const unsigned char* k) { return bench::MtSignature::nodeId(k); }
bool adm(const unsigned char* k) { unsigned char sc[32] = {}; return bench::MtSignature::admissiblePublicKey(k, sc); }
IdentityBinding loc(const unsigned char* p, unsigned n) { IdentityBinding b{}; b.version = kVersion; b.algorithm = kAlgo; b.generation = 1; b.node = n; std::memcpy(b.publicKey, p, 32); return b; }
PkiPeerPin pin(unsigned n, const unsigned char* p) { PkiPeerPin r; r.node = n; std::memcpy(r.publicKey, p, 32); return r; }
void eq(const PkiPeerPin* x, unsigned n, const unsigned char* p) { CK(x); if (!x) return; CK(x->node == n); CK(!std::memcmp(x->publicKey, p, 32)); }
}  // namespace

int main() {
  unsigned char L[32], A[32], B[32], C[32], D[32];
  pub(SL, L); pub(S1, A); pub(S2, B); pub(S3, C); pub(S4, D);
  const unsigned nL = nodeId(L), nA = nodeId(A), nB = nodeId(B), nC = nodeId(C), nD = nodeId(D);
  const unsigned char Z[32] = {};
  unsigned char NC[32]; std::memcpy(NC, A, 32); NC[31] |= 0x80;
  const IdentityBinding me = loc(L, nL);

  // Fixture sanity: derived keys admissible, bad keys rejected, peer nodes distinct.
  CK(adm(L) && adm(A) && adm(B) && adm(C) && adm(D));
  CK(!adm(Z) && !adm(NC));
  CK(nA != nB && nA != nC && nA != nD && nB != nC && nB != nD && nC != nD);

  PkiPeerSet s;
  CK(s.count() == 0); CK(!s.at(0)); CK(!s.find(nA));

  // 1 peer.
  const PkiPeerPin P1[1] = {pin(nA, A)};
  CK(s.assign(me, P1, 1)); CK(s.count() == 1);
  eq(s.at(0), nA, A); CK(!s.at(1)); CK(s.find(nA)); CK(!s.find(nB));

  // 2 peers.
  const PkiPeerPin P2[2] = {pin(nA, A), pin(nB, B)};
  CK(s.assign(me, P2, 2)); CK(s.count() == 2);
  eq(s.at(0), nA, A); eq(s.at(1), nB, B); CK(!s.at(2));

  // Four peers remain a supported subset of the sixteen-contact limit.
  const PkiPeerPin P4[4] = {pin(nA, A), pin(nB, B), pin(nC, C), pin(nD, D)};
  CK(s.assign(me, P4, 4)); CK(s.count() == 4);
  eq(s.at(3), nD, D); CK(!s.at(4)); CK(!s.at(PkiPeerSet::Limit));
  CK(s.find(nD)); CK(!s.find(0));

  // Copy semantics: mutating the caller's array after assign must not alias.
  PkiPeerPin M = pin(nA, A); const PkiPeerPin* Am = &M;
  CK(s.assign(me, Am, 1)); CK(s.count() == 1);
  M.node = 0xDEADBEEF; std::memset(M.publicKey, 0xA5, 32);
  eq(s.at(0), nA, A); CK(s.find(nA));

  // Reject: count 0 / over Limit / duplicate fifth pin / null pins.
  const PkiPeerPin P5[5] = {pin(nA, A), pin(nB, B), pin(nC, C), pin(nD, D), pin(nA, A)};
  CK(!s.assign(me, nullptr, 0)); CK(!s.assign(me, P4, 0));
  CK(!s.assign(me, P5, 5)); CK(!s.assign(me, nullptr, 3));
  const PkiPeerPin tooMany[PkiPeerSet::Limit+1]{};
  CK(!s.assign(me,tooMany,PkiPeerSet::Limit+1));
  CK(s.count() == 1); eq(s.at(0), nA, A);

  // Reject: invalid local identity fields (set preserved on failure).
  IdentityBinding bv = me; bv.version = kVersion + 1; CK(!s.assign(bv, P4, 4));
  IdentityBinding ba = me; ba.algorithm = kAlgo + 1; CK(!s.assign(ba, P4, 4));
  IdentityBinding bg = me; bg.generation = 0; CK(!s.assign(bg, P4, 4));
  IdentityBinding bn = me; bn.node = nL + 1; CK(!s.assign(bn, P4, 4));
  IdentityBinding bk = me; std::memcpy(bk.publicKey, Z, 32); bk.node = nodeId(Z);
  CK(!s.assign(bk, P4, 4));
  CK(s.count() == 1); eq(s.at(0), nA, A);

  // Reject: invalid / reserved / mismatched peer node.
  PkiPeerPin r0 = pin(0, B); CK(!s.assign(me, &r0, 1));
  PkiPeerPin rm = pin(nB + 1, B); CK(!s.assign(me, &rm, 1));

  for (unsigned reserved : {1u, 0xfffffffeu, 0xffffffffu}) {
    PkiPeerPin bad = pin(reserved, B); CK(!s.assign(me, &bad, 1));
    CK(s.count() == 1); eq(s.at(0), nA, A);
  }
  PkiPeerPin selfNode = pin(nL, B); CK(!s.assign(me, &selfNode, 1));
  PkiPeerPin selfKey = pin(nB, L); CK(!s.assign(me, &selfKey, 1));
  PkiPeerPin self = pin(nL, L); CK(!s.assign(me, &self, 1));
  CK(s.count() == 1); eq(s.at(0), nA, A);

  // Reject: low-order / non-canonical peer key.
  PkiPeerPin rl = pin(nodeId(Z), Z); CK(!s.assign(me, &rl, 1));
  PkiPeerPin rn = pin(nodeId(NC), NC); CK(!s.assign(me, &rn, 1));

  // Reject: duplicate peer nodes / duplicate full keys.
  const PkiPeerPin DN[2] = {pin(nA, A), pin(nA, B)}; CK(!s.assign(me, DN, 2));
  const PkiPeerPin DK[2] = {pin(nA, A), pin(nA, A)}; CK(!s.assign(me, DK, 2));
  CK(s.count() == 1);

  // clear(): empty set, find/at return null.
  s.clear();
  CK(s.count() == 0); CK(!s.at(0)); CK(!s.at(PkiPeerSet::Limit - 1));
  CK(!s.find(nA)); CK(!s.find(0));

  std::printf("peer_set_cases: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
  if (!fails) std::printf("PASS strict-peer-set\n");
  return fails ? 1 : 0;
}
