// SPDX-License-Identifier: GPL-3.0-only
// Exact native driver body is concatenated before this fixture by the runner.
#include "IdleRx.h"
#include <initializer_list>
struct ServiceState {
  bool rx=true,fault=false,activeTx=false;
  bool receiving(){return rx;}bool faulted(){return fault;}bool active(){return activeTx;}
};
struct NativeState {
  bool readyState=true;int healthState=1;
  unsigned readyCalls=0,healthCalls=0,busyCalls=0;
  bool ready(){++readyCalls;return readyState;}
  int health(){++healthCalls;return healthState;}
  bool busy(){++busyCalls;reads=0;return sx126x_rx_activity(&dev)!=0;}
};
int main(){
  unsigned gates=0;
  for(bool rx:{false,true})for(bool fault:{false,true})for(bool active:{false,true})
  for(bool ready:{false,true})for(int health:{-5,0,1}){
    boot(0);ServiceState s{rx,fault,active};NativeState r;r.readyState=ready;r.healthState=health;
    mt::observeIdleRx(s,r);
    assert(r.readyCalls==unsigned(rx&&!fault&&!active));
    assert(r.healthCalls==unsigned(rx&&!fault&&!active&&ready));
    assert(r.busyCalls==unsigned(rx&&!fault&&!active&&ready&&health==1));++gates;
  }
  // No background observation: the first later save sees a raw flag for the
  // first time and must defer, regardless of its real unobserved on-air age.
  boot(SX126X_IRQ_PREAMBLE_DETECTED);now_ms=500000;assert(sx126x_rx_activity(&dev)==1&&clears==0);
  // Idle observation ages the existing bounded state before the later save.
  boot(SX126X_IRQ_PREAMBLE_DETECTED);ServiceState s;NativeState r;
  mt::observeIdleRx(s,r);assert(data.preamble_seen_at_ms==1000&&clears==0);
  now_ms=1050;mt::observeIdleRx(s,r);assert(clears==1&&irq==0);
  now_ms=1060;reads=0;assert(sx126x_rx_activity(&dev)==0); // fresh save-time check
  // Maintenance never authorizes a cached-clear save across new activity.
  irq=SX126X_IRQ_HEADER_VALID;reads=0;assert(sx126x_rx_activity(&dev)==1&&irq==SX126X_IRQ_HEADER_VALID);
  boot(SX126X_IRQ_RX_DONE);mt::observeIdleRx(s,r);assert(submits==1&&clears==0&&(irq&SX126X_IRQ_RX_DONE));
  boot(0);chip_busy=1;mt::observeIdleRx(s,r);assert(reads==0&&clears==0);
  printf("PASS idle RX maintenance: %u gate combinations; idle-first-observation reproduction; fresh save check; RX_DONE preserved\n",gates);
}
