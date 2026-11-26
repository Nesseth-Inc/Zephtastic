// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace mt::tcp {
struct Packet {uint32_t session=0;uint16_t length=0;uint8_t bytes[512]{};};
// Explicit WiFi build, verified enabled boot policy only. Default has no socket
// or thread. All callbacks and IO stay outside the application service mutex.
void init(bool enabled);
bool connected();
uint32_t session();
bool receive(Packet &packet);
// Called with serviceLock already held. Takes only the transport state mutex;
// the callback may reenter send, never socket IO. Network owner takes no
// serviceLock. A stale packet is rejected at this acceptance boundary.
bool dispatch(const Packet &,void(*callback)(const Packet &,void *),void *context);
bool send(const uint8_t *framed,size_t bytes,uint32_t session,bool idleOnly=false);
}
