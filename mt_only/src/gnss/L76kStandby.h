// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace mt::gnss {
// Pinned Meshtastic GPS::writePinStandby uses active-low standby on Seeed's
// L76K. Zephyr's GPIO spec applies polarity. Leave the module asleep on error.
template<class Standby, class Receiver>
bool setL76kAcquiring(bool on, Standby standby, Receiver receiver)
{
    if (on && receiver(true) == 0 && standby(false) == 0)
        return true;
    const int sleep = standby(true);
    const int close = receiver(false);
    return !on && sleep == 0 && close == 0;
}
}
