// SPDX-License-Identifier: GPL-3.0-only
#define CONFIG_ZEPHCORE_CONTINUOUS_DUAL_REPEATER 1
#define CONFIG_ZEPHCORE_MT_ROUTING 1
#include "../../patches/zephyr-new/include/zephyr/drivers/lora/sx126x_dual_gate.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
static void packet(uint8_t *p) {
 memset(p,0x55,64);put32(p,0x12345678);put32(p+4,0xabcdef01);put32(p+8,1);
 p[12]=0x6a;p[13]=0;p[14]=0x7b;p[15]=0x7b;
}
int main() {
 uint8_t p[64];packet(p);assert(cd_mt_frame(p,64,0x7b));
 p[13]=0xf1;assert(cd_mt_frame(p,64,0x7b)); // Opaque unknown-channel transit.
 p[12]=0x68;assert(cd_mt_frame(p,64,0x7b)); // Final zero-hop copy is legal.
 p[12]=0x6b;assert(!cd_mt_frame(p,64,0x7b)); // Transit must decrement hop.
 packet(p);put32(p,0xfffffffe);assert(!cd_mt_frame(p,64,0x7b));
 put32(p,0);assert(!cd_mt_frame(p,64,0x7b));
 packet(p);put32(p,0xffffffff);assert(!cd_mt_frame(p,64,0x7b));
 p[12]&=uint8_t(~8U);assert(cd_mt_frame(p,64,0x7b));
 packet(p);put32(p+4,0);assert(!cd_mt_frame(p,64,0x7b));
 put32(p+4,0xffffffff);assert(!cd_mt_frame(p,64,0x7b));
 packet(p);put32(p+8,0);assert(!cd_mt_frame(p,64,0x7b));
 packet(p);p[15]^=1;assert(!cd_mt_frame(p,64,0x7b));
 packet(p);assert(!cd_mt_frame(p,16,0x7b)&&!cd_mt_frame(p,256,0x7b));
 for(unsigned changed=0;changed<16;++changed) {
  sx126x_dual_gate g{};const uint8_t identity[3]={1,2,3};packet(p);
  assert(dg_arm(&g,0x7b,identity,1,0,0));assert(dg_issue(&g,1,p,64,1,0,100));
  p[changed]^=1;assert(!dg_admit(&g,1,p,64,1,0));assert(g.ledger.used[1]==0);
  p[changed]^=1;assert(dg_issue(&g,1,p,64,1,0,100));
  assert(!dg_admit(&g,0,p,64,1,0));assert(g.ledger.used[0]+g.ledger.used[1]==0);
  assert(dg_issue(&g,1,p,64,1,0,100));assert(dg_admit(&g,1,p,64,1,0));
  assert(!dg_ready(&g,0,1,0)&&dg_ready(&g,1,1,0));
  assert(dg_complete(&g,1,false));assert(g.ledger.used[1]==351);
 }
 puts("PASS routing native gate: unicast/opaque/ACK flags/hop bounds,exact header/device/owner binding,no cross-network grant,conserved budget");
}
