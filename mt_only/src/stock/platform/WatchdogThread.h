// SPDX-License-Identifier: GPL-3.0-only
// Meshtastic 8e6a88d src/watchdog/watchdogThread.cpp; external DONE/WAKE pins.
#pragma once
namespace mt::stock::watchdog {
template<class Pins> bool feedDog(Pins &pins) {
    if(!pins.done(true))return false;
    pins.delayMs(1);
    return pins.done(false);
}
template<class Pins> bool setup(Pins &pins) {
    if(!pins.configure())return false;
    pins.delayMs(1);
    if(!pins.done(false))return false;
    pins.delayMs(1);
    return feedDog(pins);
}
}
