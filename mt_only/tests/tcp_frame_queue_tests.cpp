// SPDX-License-Identifier: GPL-3.0-only
// mt_tcp_txq_fixture.cpp — assert-based C++17 fixture for locked mt::tcp::TxQueue; link against the locked impl.
#include "network/MtTcpTxQueue.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
using mt::tcp::TxQueue; using Frame = mt::tcp::TxQueue::Frame;

size_t makeFramed(uint8_t *out, size_t bodyLen) {
  out[0]=0x94; out[1]=0xc3;
  out[2]=static_cast<uint8_t>(bodyLen>>8); out[3]=static_cast<uint8_t>(bodyLen);
  for (size_t i=0;i<bodyLen;i++) out[4+i]=static_cast<uint8_t>(i*31u+17u);
  return 4+bodyLen;
}

bool sameFrame(const Frame &f, const uint8_t *ref, size_t n) {
  return f.length==static_cast<uint16_t>(n) && std::memcmp(f.bytes,ref,n)==0;
}

void runCases() {
  TxQueue q;
  // (1) valid minimum and maximum frames enqueue/pop byte-identically
  uint8_t minF[516], maxF[516];
  size_t minN=makeFramed(minF,1), maxN=makeFramed(maxF,512);
  assert(q.pushFramed(minF,minN) && q.pushFramed(maxF,maxN));
  Frame a{}, b{};
  assert(q.pop(a) && sameFrame(a,minF,minN));
  assert(q.pop(b) && sameFrame(b,maxF,maxN));
  assert(!q.pop(a) && !q.busy());
  // (2) malformed prefix, zero/mismatched length, null, >516: rejected, queue unchanged
  q.pushFramed(minF,minN);
  uint8_t bad[520];
  std::memcpy(bad,minF,minN);
  bad[0]=0x95;
  assert(!q.pushFramed(bad,minN));
  bad[0]=0x94; bad[2]=0x00; bad[3]=0x00;
  assert(!q.pushFramed(bad,minN));
  std::memcpy(bad,minF,minN);
  bad[2]=0x00; bad[3]=0x10;
  assert(!q.pushFramed(bad,minN));
  assert(!q.pushFramed(nullptr,0));
  assert(!q.pushFramed(bad,517));
  Frame c{};
  assert(q.pop(c) && sameFrame(c,minF,minN));
  assert(!q.pop(c));
  // (3) fill 4, reject 5th with busy, FIFO preserved, busy clears at low-water
  uint8_t f0[516], f1[516], f2[516], f3[516], f4[516];
  size_t n0=makeFramed(f0,10),n1=makeFramed(f1,20),n2=makeFramed(f2,30),n3=makeFramed(f3,40),n4=makeFramed(f4,50);
  assert(q.pushFramed(f0,n0)&&q.pushFramed(f1,n1)&&q.pushFramed(f2,n2)&&q.pushFramed(f3,n3));
  assert(!q.pushFramed(f4,n4) && q.busy());
  assert(q.pop(a) && sameFrame(a,f0,n0));
  assert(q.busy());
  assert(q.pop(b) && sameFrame(b,f1,n1));
  assert(q.busy());
  assert(q.pop(c) && sameFrame(c,f2,n2));
  assert(!q.busy());
  // (4) reset empties prior session; a new frame works
  q.reset();
  assert(!q.pop(a) && !q.busy());
  assert(q.pushFramed(f0,n0) && q.pop(a) && sameFrame(a,f0,n0));
}
}

int main() { runCases(); return 0; }
