// SPDX-License-Identifier: GPL-3.0-only
#ifndef MT_BLE_SETTINGS_NAMESPACE_H
#define MT_BLE_SETTINGS_NAMESPACE_H

#include <stddef.h>

namespace mt {
namespace ble {

// Prefix classifier only; not a complete key validator.
// Returns false when key is null or length < 3. Otherwise returns true
// iff the first three bytes are exactly the ASCII bytes 'b', 't', '/'.
// Inspects no byte at index >= length and requires no NUL terminator.
inline bool hasBluetoothSettingsPrefix(const char *key, size_t length)
{
    if (key == nullptr) {
        return false;
    }
    if (length < 3) {
        return false;
    }
    return key[0] == 'b' && key[1] == 't' && key[2] == '/';
}

}  // namespace ble
}  // namespace mt

#endif  // MT_BLE_SETTINGS_NAMESPACE_H
