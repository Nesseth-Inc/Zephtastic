// SPDX-License-Identifier: GPL-3.0-only
// Deterministic mixed valid/malformed state-machine load; no radio or secrets.
#include "../../adapters/bench/vendor/RoutingCore.h"
#include <cassert>
#include <cstdio>
using namespace bench;
static uint32_t randomState=0xb286ec91;
static uint32_t randomWord() {randomState^=randomState<<13;randomState^=randomState>>17;randomState^=randomState<<5;return randomState;}
static void put(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8));}
static uint32_t airtime(size_t bytes,void *) {return uint32_t(bytes*7+100);}
int main() {
 RoutingCore c;c.configureContinuous(0x7b,8);c.configureIdentity(0x1234567b);
 c.setOtherOpaqueChannels(true);c.configureAirtime(airtime);c.configureTiming(28,100,4500);
 uint32_t now=0xffff0000,accepted=0,transmitted=0;uint8_t p[281];
 for(unsigned i=0;i<250000;++i) {
  for(auto &b:p)b=uint8_t(randomWord());
  size_t n=randomWord()%281;
  RouteMetadata metadata;
  if(i%3==0) {
   n=17+randomWord()%239;put(p,0x34567890);put(p+4,0x87654321);put(p+8,1+i/9);
   const uint8_t start=1+randomWord()%7,hop=randomWord()%(start+1);
   p[12]=uint8_t((start<<5)|hop|(randomWord()&8));p[13]=(i%2)?8:0;
   p[14]=uint8_t((i%5)==0 ? 0x7b:0);p[15]=0x21;
   if(i%2) {metadata.channelDecoded=true;metadata.portnum=1;metadata.encodedDataSize=n-16;}
  }
  if(i%13==0)metadata.decodeRejected=true;
  const Protocol protocol=(i%11)==0 ? Protocol::MeshCore:Protocol::Meshtastic;
  bool admitted=c.ingestWithMetadata(protocol,(i%31)==0?nullptr:p,n,metadata,now,randomWord());
  if(protocol==Protocol::MeshCore)assert(!admitted);
  accepted+=admitted;now+=13;c.expire(now);
  if(i%3==0) if(auto *q=c.next(now)) {
   c.noteTransmitStart(*q);Frame active=*q;q->used=false;++active.attempts;
   assert(active.attempts<=3);c.complete(active,now,(i%17)!=0);++transmitted;
  }
  if(i%101==0)c.noteBlockedAirtime(150);
  assert(c.pending()<=8&&c.retransmitting()<=8&&c.opaqueCount()<=64&&c.historyCount()<=64&&c.knownCount()<=32&&c.routeCount()<=16);
 }
 c.clearPending();assert(!c.pending()&&!c.retransmitting());
 assert(accepted>100&&transmitted>100);
 std::printf("PASS250000 mixed valid/malformed/protocol/key-metadata state inputs,wrap,capacity,failed TX;accepted=%u completed=%u core_bytes=%zu\n",accepted,transmitted,sizeof(c));
}
