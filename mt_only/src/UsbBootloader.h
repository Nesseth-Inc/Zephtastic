// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
struct device;
namespace mt::usbBootloader {
// Called only by main while serviceLock is held. Once true, callers must skip
// new client/RF work until reset. No USB callback performs settings IO or reset.
bool poll(const device *usb,uint64_t now,void (*stop)(),void (*beforeReset)());
}
