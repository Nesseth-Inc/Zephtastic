// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt::pki {
// Volatile boot totals under the existing Service lock. No packet IDs, keys,
// plaintext, persistence, policy decisions or claim of completed transmission.
struct PrivateReplyDiagnostics {
  uint32_t attempts=0,queued=0,no_slot=0,producer=0,id=0,frame=0,queue=0;
  static void bump(uint32_t &value){if(value!=UINT32_MAX)++value;}
  static uint32_t sum(uint32_t a,uint32_t b){return b>UINT32_MAX-a?UINT32_MAX:a+b;}
  void add(const PrivateReplyDiagnostics &b){
    attempts=sum(attempts,b.attempts);queued=sum(queued,b.queued);no_slot=sum(no_slot,b.no_slot);
    producer=sum(producer,b.producer);id=sum(id,b.id);frame=sum(frame,b.frame);queue=sum(queue,b.queue);
  }
};
}
