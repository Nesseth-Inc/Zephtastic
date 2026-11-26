// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "AmbientLightingCore.h"
#include "PaxcounterCore.h"

namespace mt::modules::peripherals::zephyr {
void init();
bool ambientAvailable();
int applyAmbient(void *context, uint8_t current, uint8_t red, uint8_t green, uint8_t blue);
int ambientOff(void *context);
// The owner loop starts/stops only while Paxcounter is enabled. A scanner
// started by another Zephyr owner is observed but never stopped by this module.
bool startBleObserver(bool paxcounterEnabled);
void stopBleObserver();
void startWifiObserver(bool paxcounterEnabled);
void stopWifiObserver();
#if defined(CONFIG_MT_PAXCOUNTER_WIFI_ESP32)
void capturePax(bool enabled);
#endif
size_t drainPaxObservations(PaxcounterCore &core);
uint32_t observationQueueDrops();
// Capability means this module has registered a receiver or owns a passive
// scan/monitor lease. It does not mean that any address has been observed.
bool wifiObservationAvailable();
bool bleObservationAvailable();
} // namespace mt::modules::peripherals::zephyr
