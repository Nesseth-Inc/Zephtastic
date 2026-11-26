// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
namespace mt::pki {
struct KeyRef {uint32_t slot=0,generation=0;};
struct PrivateKeyOperation {
  virtual ~PrivateKeyOperation()=default;
  // Trusted in-process operation only. The pointer is valid only during this
  // callback and must not be retained. No transport/export endpoint is provided.
  virtual bool apply(const uint8_t scalar[32])=0;
};
struct KeyProvider {
  virtual ~KeyProvider()=default;
  // Invoke only for the exact active generation. No key generation, persistence,
  // defaults or external raw-key getter here. False leaves caller failing closed.
  virtual bool active(KeyRef)const=0; // Cheap revocation check, never exposes a key.
  virtual bool withPrivate(KeyRef,PrivateKeyOperation &)=0;
};
}
