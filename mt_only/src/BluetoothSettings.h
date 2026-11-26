// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mt {

// BluetoothSettings: desired (not runtime) Bluetooth pairing configuration.
//
// Canonical 12-byte wire layout (fixedPin little-endian):
//   [0]='Z' [1]='B' [2]=1 [3]=enabled(0/1) [4]=mode [5..7]=reserved(0) [8..11]=fixedPin LE
//   mode: 0=RandomPin, 1=FixedPin, 2=NoPin. fixedPin valid range 0..999999 in every mode.
//
// Stock parity (config.pb.h, pinned stock 8e6a88d):
//   typedef struct _meshtastic_Config_BluetoothConfig {
//       bool enabled;
//       meshtastic_Config_BluetoothConfig_PairingMode mode;
//       uint32_t fixed_pin;
//   } meshtastic_Config_BluetoothConfig;
// Stock Admin SET copies all three fields; stock GET returns the whole struct.
// This helper stores desired configuration, not runtime readiness
// (M101 currently returns host_.bluetooth() and hardcodes NO_PIN).
// Mapping source/pins: ../m102-stock-module-parity-research-r1/packets/02-ble-config.txt,
// SOURCE-PINS.json.

struct BluetoothSettings {
    enum : uint8_t { RandomPin = 0, FixedPin = 1, NoPin = 2 };

    bool enabled = true;
    uint8_t mode = NoPin;
    uint32_t fixedPin = 0;

    static constexpr size_t Size = 12;

    // Deterministic defaults: enabled, NoPin, fixedPin 0.
    static BluetoothSettings defaults() {
        BluetoothSettings s;
        s.enabled = true;
        s.mode = NoPin;
        s.fixedPin = 0;
        return s;
    }

    // Semantic validation. hasDisplay=false rejects RandomPin (needs a display).
    // fixedPin numeric range (0..999999) is enforced in every mode.
    static bool valid(const BluetoothSettings& s, bool hasDisplay = true) {
        if (s.mode != RandomPin && s.mode != FixedPin && s.mode != NoPin) {
            return false;  // unknown mode
        }
        if (!hasDisplay && s.mode == RandomPin) {
            return false;  // RandomPin requires a display
        }
        if (s.fixedPin > 999999u) {
            return false;  // numeric PIN range, every mode
        }
        return true;
    }

    // Full-field equality.
    bool same(const BluetoothSettings& other) const {
        return enabled == other.enabled &&
               mode == other.mode &&
               fixedPin == other.fixedPin;
    }

    // Atomic: out is left unchanged on any failure.
    static bool decode(const uint8_t* buf, size_t len, BluetoothSettings& out) {
        if (buf == nullptr) {
            return false;
        }
        if (len != Size) {
            return false;  // wrong size
        }
        if (buf[0] != 'Z' || buf[1] != 'B' || buf[2] != 1) {
            return false;  // bad magic
        }
        if (buf[3] != 0 && buf[3] != 1) {
            return false;  // invalid bool
        }
        if (buf[5] != 0 || buf[6] != 0 || buf[7] != 0) {
            return false;  // nonzero reserved bytes
        }
        BluetoothSettings s;
        s.enabled = (buf[3] == 1);
        s.mode = buf[4];
        s.fixedPin = static_cast<uint32_t>(buf[8]) |
                     (static_cast<uint32_t>(buf[9]) << 8) |
                     (static_cast<uint32_t>(buf[10]) << 16) |
                     (static_cast<uint32_t>(buf[11]) << 24);
        if (!valid(s, true)) {  // capability-neutral codec validation
            return false;
        }
        out = s;
        return true;
    }

    // Atomic: buf and written are left unchanged on any failure.
    // fixedPin is written in every mode (stock GET roundtrip preservation).
    static bool encode(const BluetoothSettings& s, uint8_t* buf, size_t cap, size_t& written) {
        if (buf == nullptr) {
            return false;
        }
        if (cap < Size) {
            return false;  // bad capacity
        }
        if (!valid(s, true)) {  // capability-neutral codec validation
            return false;
        }
        buf[0] = 'Z';
        buf[1] = 'B';
        buf[2] = 1;
        buf[3] = s.enabled ? 1 : 0;
        buf[4] = s.mode;
        buf[5] = 0;
        buf[6] = 0;
        buf[7] = 0;
        buf[8] = static_cast<uint8_t>(s.fixedPin & 0xFFu);
        buf[9] = static_cast<uint8_t>((s.fixedPin >> 8) & 0xFFu);
        buf[10] = static_cast<uint8_t>((s.fixedPin >> 16) & 0xFFu);
        buf[11] = static_cast<uint8_t>((s.fixedPin >> 24) & 0xFFu);
        written = Size;
        return true;
    }
};

}  // namespace mt
