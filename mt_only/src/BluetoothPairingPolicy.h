// SPDX-License-Identifier: GPL-3.0-only
// BluetoothPairingPolicy.h
// M102 pairing policy resolution.
//
// resolve() computes the *requested* pairing policy from persisted
// BluetoothSettings. It is not proof that pairing, bond creation, or
// security negotiation succeeded.
//
// Truth table:
//   invalid settings (BluetoothSettings::valid == false)
//       -> false, out unchanged
//   valid, disabled
//       -> true, default empty plan (even if the BT settings store is
//          unavailable)
//   enabled, NoPin
//       -> advertise, Encrypted, non-bondable, no passkey, no display
//          (preserves the M101 encrypted-L2/non-bonding behavior;
//          deliberately distinct from stock no-PIN backends)
//   enabled, FixedPin
//       -> requires btSettingsReady; advertise, bondable, Authenticated,
//          fixed passkey; no display required
//   enabled, RandomPin
//       -> requires btSettingsReady and hasDisplay; advertise, bondable,
//          Authenticated, random passkey, displayPasskey
//
// Rejections (out unchanged):
//   FixedPin/RandomPin while the BT settings store is unavailable
//   RandomPin without a display
// Authenticated modes are never downgraded: an unavailable store rejects
// instead of weakening to Encrypted.

#pragma once

#include <stdint.h>

#include "BluetoothSettings.h"

namespace mt {
namespace ble {

enum class Security : uint8_t {
    None,
    Encrypted,
    Authenticated,
};

enum class PasskeySource : uint8_t {
    None,
    Fixed,
    Random,
};

struct BluetoothPairingPlan {
    bool advertise = false;
    bool bondable = false;
    bool displayPasskey = false;
    Security security = Security::None;
    PasskeySource passkeySource = PasskeySource::None;
};

// Resolve the requested pairing policy into `out`.
// Returns false without modifying `out` when settings are invalid, when a
// PIN mode is requested but the BT settings store is unavailable, or when a
// random passkey is requested without a display.
inline bool resolve(const mt::BluetoothSettings& settings,
                    bool hasDisplay,
                    bool btSettingsReady,
                    BluetoothPairingPlan& out) {
    if (!mt::BluetoothSettings::valid(settings, hasDisplay)) {
        return false;
    }
    if (!settings.enabled) {
        out = BluetoothPairingPlan{};
        return true;
    }
    BluetoothPairingPlan plan;
    switch (settings.mode) {
        case mt::BluetoothSettings::NoPin:
            plan.advertise = true;
            plan.security = Security::Encrypted;
            out = plan;
            return true;
        case mt::BluetoothSettings::FixedPin:
            if (!btSettingsReady) {
                return false;
            }
            plan.advertise = true;
            plan.bondable = true;
            plan.security = Security::Authenticated;
            plan.passkeySource = PasskeySource::Fixed;
            out = plan;
            return true;
        case mt::BluetoothSettings::RandomPin:
            if (!btSettingsReady || !hasDisplay) {
                return false;
            }
            plan.advertise = true;
            plan.bondable = true;
            plan.security = Security::Authenticated;
            plan.passkeySource = PasskeySource::Random;
            plan.displayPasskey = true;
            out = plan;
            return true;
        default:
            return false;
    }
}

// This stage authenticates only the current connection. The app journal
// persists desired policy; neither it nor successful SMP creates a stored bond.
// Keep resolve() above unchanged for a separately qualified persistent stage.
inline bool resolveTransient(const mt::BluetoothSettings &settings,
                             bool hasDisplay,BluetoothPairingPlan &out) {
    if(!mt::BluetoothSettings::valid(settings,hasDisplay))return false;
    BluetoothPairingPlan plan;
    if(settings.enabled){
        plan.advertise=true;
        if(settings.mode==mt::BluetoothSettings::NoPin)plan.security=Security::Encrypted;
        else {
            plan.security=Security::Authenticated;
            plan.passkeySource=settings.mode==mt::BluetoothSettings::FixedPin?
                PasskeySource::Fixed:PasskeySource::Random;
            plan.displayPasskey=settings.mode==mt::BluetoothSettings::RandomPin;
        }
    }
    out=plan;return true;
}

} // namespace ble
} // namespace mt
