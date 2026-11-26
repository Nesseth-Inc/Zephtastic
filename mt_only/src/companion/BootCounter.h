// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stdint.h>

namespace mt::companion {
// Called once during ESP startup, after reset recovery has admitted settings.
template<class Store> uint32_t incrementBootCounter(Store &store) {
    constexpr char key[]="mtinfo/boots";
    uint32_t value=0;
    int size=store.read(key,reinterpret_cast<uint8_t*>(&value),sizeof(value));
    if(size!=-ENOENT && size!=int(sizeof(value)))return 0;
    ++value;
    if(store.write(key,reinterpret_cast<const uint8_t*>(&value),sizeof(value)))return 0;
    return value;
}
}
