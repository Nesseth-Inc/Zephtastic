// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace mt::update {
bool signedBleAvailable();
// The reconciled update owner has closed ingress and quiesced radio/storage.
// progress services the watchdog and grants/settles native Bluetooth settings.
void serveSignedBle(const uint8_t expected[32],void (*progress)());
}
