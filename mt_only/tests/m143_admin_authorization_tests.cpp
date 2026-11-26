// SPDX-License-Identifier: GPL-3.0-only
// Synthetic allowlist tests only; no real identity, device, RF, or stored key.
#include "admin/AdminAuthorization.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using mt::admin::AdminAuthorization;
using mt::admin::Authorization;
using mt::pki::PrivateEvent;
using Security = meshtastic_Config_SecurityConfig;

constexpr uint32_t Local = 0x12345678u;
constexpr uint32_t Peer = 0x23456789u;

Security emptySecurity() { return meshtastic_Config_SecurityConfig_init_zero; }

PrivateEvent authenticatedEvent(uint32_t from = Peer, uint32_t to = Local,
                                uint32_t id = 17, uint8_t keyByte = 0x31) {
  PrivateEvent event{};
  event.from = from;
  event.to = to;
  event.id = id;
  event.size = 1;
  event.data[0] = 0x0a;
  std::memset(event.publicKey, keyByte, sizeof(event.publicKey));
  return event;
}

void setKey(Security &security, size_t slot, uint8_t value) {
  assert(slot < AdminAuthorization::MaximumAdminKeys);
  security.admin_key[slot].size = AdminAuthorization::AdminKeyBytes;
  std::memset(security.admin_key[slot].bytes, value,
              AdminAuthorization::AdminKeyBytes);
}

void allowsOnlyCurrentlyConfiguredAdminKeys() {
  auto security = emptySecurity();
  security.admin_key_count = 2;
  setKey(security, 0, 0x31);
  setKey(security, 1, 0x52);

  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer, Local, 17, 0x31), Local, security) ==
         Authorization::Authorized);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer + 1, Local, 18, 0x52), Local, security) ==
         Authorization::Authorized);
  security.admin_key_count = 3;
  setKey(security, 2, 0x64);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer + 2, Local, 19, 0x64), Local, security) ==
         Authorization::Authorized);
  security.admin_key_count = 2;

  // A separately known or pinned peer does not gain Admin authority.
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer + 2, Local, 19, 0x73), Local, security) ==
         Authorization::Denied);

  // Authorization uses the supplied live config; a replaced key has no
  // cached grant, and clearing the allowlist fails closed.
  setKey(security, 0, 0x42);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer, Local, 17, 0x52), Local, security) ==
         Authorization::Authorized);  // slot 1 is still explicitly authorized
  security.admin_key[1].size = AdminAuthorization::AdminKeyBytes;
  std::memset(security.admin_key[1].bytes, 0x42,
              AdminAuthorization::AdminKeyBytes);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer, Local, 17, 0x52), Local, security) ==
         Authorization::Denied);
  security.admin_key_count = 0;
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(), Local, security) ==
         Authorization::InvalidAdminKeyConfig);
}

void malformedAdminKeyConfigurationFailsClosed() {
  auto security = emptySecurity();
  security.admin_key_count = 1;
  setKey(security, 0, 0x31);
  const auto event = authenticatedEvent();

  security.admin_key_count = AdminAuthorization::MaximumAdminKeys + 1;
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidAdminKeyConfig);

  security.admin_key_count = 1;
  security.admin_key[0].size = 31;
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidAdminKeyConfig);
  security.admin_key[0].size = 33;  // nanopb's size field can report oversize
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidAdminKeyConfig);
  security.admin_key_count = 2;
  security.admin_key[0].size = AdminAuthorization::AdminKeyBytes;
  setKey(security, 0, 0x31);
  security.admin_key[1].size = 31;
  assert(AdminAuthorization::authorizeAuthenticatedEvent(
             authenticatedEvent(Peer, Local, 17, 0x31), Local, security) ==
         Authorization::InvalidAdminKeyConfig);
  security.admin_key_count = 1;
  security.admin_key[0].size = AdminAuthorization::AdminKeyBytes;
  std::memset(security.admin_key[0].bytes, 0,
              AdminAuthorization::AdminKeyBytes);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidAdminKeyConfig);
}

void malformedNonUnicastAndNonlocalEventsAreRejected() {
  auto security = emptySecurity();
  security.admin_key_count = 1;
  setKey(security, 0, 0x31);
  auto event = authenticatedEvent();

  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, 0, security) ==
         Authorization::InvalidEvent);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, 1, security) ==
         Authorization::InvalidEvent);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, 0xfffffffeu, security) ==
         Authorization::InvalidEvent);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, 0xffffffffu, security) ==
         Authorization::InvalidEvent);

  event = authenticatedEvent(0, Local);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent(1, Local);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent(Local, Local);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent(0xfffffffeu, Local);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent(0xffffffffu, Local);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);

  event = authenticatedEvent(Peer, 0xffffffffu);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent(Peer, Local, 0);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent(Peer, Local, 17, 0);
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event = authenticatedEvent();
  event.size = 0;
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
  event.size = sizeof(event.data) + 1;
  assert(AdminAuthorization::authorizeAuthenticatedEvent(event, Local, security) ==
         Authorization::InvalidEvent);
}
}  // namespace

int main() {
  allowsOnlyCurrentlyConfiguredAdminKeys();
  malformedAdminKeyConfigurationFailsClosed();
  malformedNonUnicastAndNonlocalEventsAreRejected();
  std::puts("PASS synthetic authenticated-event Admin allowlist controls");
}
