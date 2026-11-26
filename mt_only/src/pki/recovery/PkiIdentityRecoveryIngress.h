// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../PkiIdentity.h"

namespace mt::pki {

// Adapter boundary for the measured USB epoch. The target performs its own
// service-lock, inactive-runtime, and radio-off quiescence proof.
struct IdentityRecoveryTarget {
  virtual ~IdentityRecoveryTarget()=default;
  virtual int restoreExistingIdentity(const uint8_t scalar[32])=0;
};

struct UsbIdentityRestoreParser {
  static constexpr char Prefix[]="zt_pki_restore_v1 ";
  static constexpr size_t PrefixLength=sizeof(Prefix)-1;
  static constexpr size_t CommandLength=PrefixLength+64;

  static bool handles(const uint8_t *input,size_t size) {
    return input&&size>=PrefixLength&&!memcmp(input,Prefix,PrefixLength);
  }
  static bool parse(const uint8_t *input,size_t size,uint8_t scalar[32]) {
    if(!input||!scalar||size!=CommandLength||memcmp(input,Prefix,PrefixLength))return false;
    memset(scalar,0,32);
    for(unsigned i=0;i<32;++i) {
      const uint8_t a=input[PrefixLength+2*i],b=input[PrefixLength+2*i+1];
      auto nibble=[](uint8_t c)->int {
        if(c>='0'&&c<='9')return int(c-'0');
        if(c>='a'&&c<='f')return int(c-'a'+10);
        return -1;
      };
      const int hi=nibble(a),lo=nibble(b);
      if(hi<0||lo<0){eraseIdentityBytes(scalar,32);return false;}
      scalar[i]=uint8_t((hi<<4)|lo);
    }
    return true;
  }
};

// Explicit USB-local scalar restore. It exposes only an integer result, never
// echoes, formats, or logs the supplied private scalar. All owned secret bytes
// are wiped before returning, including on epoch loss.
template<class UsbSession>
class PkiIdentityRecoveryIngress {
  IdentityRecoveryTarget &target_;UsbSession &session_;
public:
  PkiIdentityRecoveryIngress(IdentityRecoveryTarget &target,UsbSession &session):
    target_(target),session_(session){}
  PkiIdentityRecoveryIngress(const PkiIdentityRecoveryIngress &)=delete;
  PkiIdentityRecoveryIngress &operator=(const PkiIdentityRecoveryIngress &)=delete;

  int execute(const uint8_t *input,size_t size,uint32_t epoch) {
    uint8_t scalar[32]{};
    struct Wipe {uint8_t *bytes;~Wipe(){eraseIdentityBytes(bytes,32);}} wipe{scalar};
    if(!UsbIdentityRestoreParser::parse(input,size,scalar))return -EINVAL;
    if(!session_.bind(epoch)||!session_.current(epoch))return -EACCES;
    const int result=target_.restoreExistingIdentity(scalar);
    // If the USB epoch changes during an exact-readback commit, report the
    // lost response without retrying. The underlying operation is one-shot.
    if(!session_.current(epoch))return -EACCES;
    return result;
  }
};
}
