#include "storage/CapacityProbe.h"
#include <assert.h>
#include <algorithm>
#include <stdint.h>
#include <stdio.h>

namespace {
using namespace mt::m153::capacity_probe;
struct State { uint32_t errorAt=LfsBytes; uint32_t calls=0; size_t maxRead=0; size_t maxReturn=ChunkBytes; uint32_t erasedUntil=LfsBytes; };
int32_t readFake(void *ctx,uint32_t offset,uint8_t *out,size_t requested,size_t *actual) {
  auto &s=*static_cast<State *>(ctx);++s.calls;s.maxRead=std::max(s.maxRead,requested);
  if(offset>=s.errorAt)return -EIO;
  const size_t count=std::min(requested,s.maxReturn);
  if(offset+count>s.errorAt)return -EIO;
  for(size_t i=0;i<count;++i)out[i]=(offset+i<s.erasedUntil)?0xff:0x00;
  *actual=count;return 0;
}
int32_t stalled(void *,uint32_t,uint8_t *,size_t,size_t *actual){*actual=0;return 0;}
int32_t overrun(void *,uint32_t,uint8_t *,size_t requested,size_t *actual){*actual=requested+1;return 0;}
void completeReadCountsEverySector() {
  State s; s.maxReturn=13;
  const auto r=scanLfs(readFake,&s);
  assert(r.complete&&!r.error&&r.bytes_scanned==LfsBytes&&r.sectors_complete==32);
  assert(s.maxRead<=ChunkBytes&&s.calls>LfsBytes/ChunkBytes);
  for(unsigned i=0;i<32;i++)assert(r.ff_bytes[i]==SectorBytes);
}
void sectorMapReportsOnlyNumericErasedCounts() {
  State s; s.erasedUntil=2*SectorBytes+123;
  const auto r=scanLfs(readFake,&s);
  assert(r.complete&&r.ff_bytes[0]==4096&&r.ff_bytes[1]==4096&&r.ff_bytes[2]==123);
  for(unsigned i=3;i<32;i++)assert(r.ff_bytes[i]==0);
}
void partialErrorDoesNotMarkSectorComplete() {
  State s; s.errorAt=2*SectorBytes+64;
  const auto r=scanLfs(readFake,&s);
  assert(!r.complete&&r.error==-EIO&&r.bytes_scanned==2*SectorBytes+64);
  assert(r.sectors_complete==2&&r.ff_bytes[0]==4096&&r.ff_bytes[1]==4096&&r.ff_bytes[2]==64);
}
void malformedReadersFailClosed() {
  assert(scanLfs(nullptr,nullptr).error==NoReader);
  assert(scanLfs(stalled,nullptr).error==NoProgress);
  assert(scanLfs(overrun,nullptr).error==BadReadCount);
}
}
int main(){completeReadCountsEverySector();sectorMapReportsOnlyNumericErasedCounts();partialErrorDoesNotMarkSectorComplete();malformedReadersFailClosed();puts("PASS_M153_BOUNDED_CAPACITY_SCAN");}
