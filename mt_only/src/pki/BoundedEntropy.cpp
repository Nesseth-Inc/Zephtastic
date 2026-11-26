// SPDX-License-Identifier: GPL-3.0-only
#include "BoundedEntropy.h"
#include "monocypher.h"
#include <string.h>
namespace mt::pki {
static_assert(__atomic_always_lock_free(sizeof(uint32_t),nullptr),"32-bit lock-free atomics required");
namespace {
bool enter(uint32_t &word) {
  uint32_t expected=0;
  // One weak compare-exchange: contention or a spurious failure refuses rather
  // than waiting. Builtins avoid a dependency on Zephyr's absent <atomic> header.
  return __atomic_compare_exchange_n(&word,&expected,1,true,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
}
struct Exit {uint32_t &word;~Exit(){__atomic_store_n(&word,0,__ATOMIC_RELEASE);}};
}
bool BoundedEntropy::ready() {
  if(!enter(entered_))return false;
  Exit exit{entered_};
  if(poisoned_||!port_.acquire())return false;
  const bool valid=port_.ready();
  if(!port_.release()){poisoned_=true;return false;}
  return valid;
}
bool BoundedEntropy::fill(uint8_t *out,size_t n) {
  if(!out||!n||n>Capacity||!enter(entered_))return false;
  Exit exit{entered_};
  if(poisoned_)return false;
  uint8_t scratch[Capacity]{};
  struct Wipe {uint8_t *p;~Wipe(){crypto_wipe(p,Capacity);}} wipe{scratch};
  const uint32_t start=port_.micros();
  if(!port_.acquire())return false;
  size_t have=0;
  bool valid=true;
  for(unsigned attempt=0;attempt<MaxReads&&have<n;++attempt) {
    if(uint32_t(port_.micros()-start)>=DeadlineUs||!port_.ready()){valid=false;break;}
    const int count=port_.read(scratch+have,n-have);
    if(count<0||size_t(count)>n-have){valid=false;break;}
    have+=size_t(count);
    if(have<n&&attempt+1<MaxReads)port_.pause();
  }
  valid=valid&&have==n&&port_.ready()&&uint32_t(port_.micros()-start)<DeadlineUs;
  // Release before publishing. An uncertain lease cleanup poisons this object;
  // no delayed retry of that release and no output from partially completed I/O.
  if(!port_.release()){poisoned_=true;return false;}
  if(!valid||uint32_t(port_.micros()-start)>=DeadlineUs)return false;
  memcpy(out,scratch,n);return true;
}
}
