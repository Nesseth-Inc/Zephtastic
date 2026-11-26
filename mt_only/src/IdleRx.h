// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace mt {
// Called under serviceLock after completed RX is drained and before local admin.
// The existing native observer ages stale status and preserves completed packets.
// A later save must still perform its own fresh busy check and storage barrier.
template<class ServiceType,class RadioType>
void observeIdleRx(ServiceType &service,RadioType &radio) {
  if(service.receiving()&&!service.faulted()&&!service.active()&&
     radio.ready()&&radio.health()==1)(void)radio.busy();
}
}
