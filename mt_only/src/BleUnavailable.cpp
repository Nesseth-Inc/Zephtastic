// SPDX-License-Identifier: GPL-3.0-only
// USB-only build. A missing transport has no session, queues, or radio events.
#include "BleTransport.h"
#include <errno.h>

namespace mt::ble {
int init(uint32_t) { return -ENOTSUP; }
int init(uint32_t, const BluetoothSettings &, bool) { return -ENOTSUP; }
int prepareBoot(uint32_t, const BluetoothSettings &, bool) { return -ENOTSUP; }
bool requestConfiguration(const BluetoothSettings &, bool) { return false; }
bool beginAdmin(uint32_t) { return false; }
bool adminBusy() { return false; }
bool stageAdminResult(const uint8_t *, size_t, uint32_t) { return false; }
bool finishAdminConfiguration(const BluetoothSettings &, bool, uint32_t, bool) { return false; }
bool ready() { return false; }
bool connected() { return false; }
bool disconnected() { return false; }
uint32_t session() { return 0; }
SecurityStatus security() { return {}; }
BlePairingEvent::Snapshot pairing() { return {}; }
bool receive(Packet &) { return false; }
bool dispatch(const Packet &, void (*)(const Packet &, void *), void *) { return false; }
bool send(const uint8_t *, size_t, uint32_t, bool) { return false; }
void setIngressOpen(bool) {}
unsigned pendingInput() { return 0; }
void poll() {}
void setDfuWindow(bool) {}
bool takeDfuRequest() { return false; }
void setActivityWake(void (*)()) {}
}
