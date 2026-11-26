// SPDX-License-Identifier: GPL-3.0-only
#include "ZephyrEntropy.h"
#include "FakeZephyr.h"
#include <stdio.h>
using namespace mt::pki;
extern "C" void mt_entropy_test_wipe(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;for(size_t i=0;i<n;++i)b[i]=0;}
static void group(const char *text){printf("PASS %s\n",text);}
static void reset(){assert(!fakeMutex.held&&!fake.irq);fake={};fakeRng.CONFIG=1;}
static bool denied(){ZephyrEntropy e;uint8_t out[32];memset(out,0xa5,32);bool ok=e.hedge(out);assert(!ok);for(auto b:out)assert(b==0xa5);return true;}
int main(){
  reset();{ZephyrEntropy e;assert(!fake.reads&&!fake.locks);uint8_t out[32];
#if defined(MT_TEST_UNSUPPORTED)
    assert(!e.hedge(out));group("unsupported PM SMP or source configuration rejects without output");return 0;
#else
    assert(e.ready()&&!fake.reads);assert(e.hedge(out));assert(fake.reads==8);assert(!fake.irq&&!fakeMutex.held);group("constructor inert non-consuming readiness and complete bounded target fill");
#endif
  }
  reset();fake.isr=true;assert(denied()&&!fake.reads&&!fake.locks);group("ISR context never enters source or kernel lease");
  reset();fake.deviceReady=false;assert(denied()&&!fake.reads);group("unready entropy device refuses");
  reset();fake.compatible=false;assert(denied()&&!fake.reads);group("substituted chosen entropy device refuses");
  reset();{ZephyrEntropyPort leased;assert(leased.acquire());ZephyrEntropy other;assert(!other.ready()&&!fake.reads);assert(leased.release()&&other.ready());}group("nonrecursive process-wide lease rejects another consumer without waiting");
  reset();fake.loseAfterRead=true;assert(denied());group("readiness lost during sample withholds bytes and releases locks");
#if defined(CONFIG_SOC_NRF52840)
  reset();fakeRng.CONFIG=0;assert(denied()&&!fake.reads);group("actual bias correction bit required");
  reset();fake.chunk=0;fake.stallTicks=true;assert(denied()&&fake.reads==64&&fake.sleeps==63&&fake.flags==0);group("nRF empty pool plus stalled clock terminates without BUSYWAIT");
  reset();fake.error=-ENOSYS;assert(denied()&&fake.reads==1&&!fake.sleeps);group("nRF unsupported ISR API never falls back to blocking reader");
#elif defined(CONFIG_SOC_SERIES_ESP32S3)
  for(unsigned mode=0;mode<4;++mode){reset();if(mode==0)fake.status=0;if(mode==1)fake.phy=0;if(mode==2)fake.apb=0;if(mode==3)fake.cpu=1000000;assert(denied()&&!fake.reads&&!fakeMutex.held);}group("S3 requires active BT PHY and supported stable clocks");
  reset();fake.lockBusy=true;assert(denied()&&!fake.reads&&!fake.unlocks);group("S3 busy PHY mutex never waits or enables radio");
  reset();fake.step=0;assert(denied()&&!fake.reads&&fake.cycles==16385&&!fake.irq);group("S3 stalled cycle counter terminates at exact inner cap");
  reset();fake.cycle=UINT32_MAX-3000;{ZephyrEntropy e;uint8_t out[4];assert(e.fill(out,4)&&fake.reads==1&&fake.cycles==7);}group("S3 wrap-safe cycle spacing read under IRQ exclusion");
  // Last: model uncertain unlock; terminal instance must not retry that lease.
  reset();fake.unlockFail=true;{ZephyrEntropy e;uint8_t out[4];memset(out,0xa5,4);assert(!e.fill(out,4)&&out[0]==0xa5);unsigned calls=fake.unlocks;assert(!e.fill(out,4)&&fake.unlocks==calls);}group("S3 uncertain unlock withholds output and never retries");
#endif
}
