// SPDX-License-Identifier: GPL-3.0-only
#include "DeviceRolePolicy.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <cstdio>

using namespace mt::DeviceRolePolicy;

int main() {
  static_assert(Role::CLIENT == 0 && Role::CLIENT_MUTE == 1 && Role::ROUTER == 2, "legacy role ids changed");
  for (uint32_t mode : {0u, 2u, 3u, 4u, 5u}) {
    assert(supportedRebroadcast(mode));
    assert(valid(Role::CLIENT, mode));
    assert(valid(Role::CLIENT_MUTE, mode));
    assert(valid(Role::ROUTER, mode) == (mode != RebroadcastMode::NONE));
  }
  assert(defaultRebroadcast(Role::CLIENT) == RebroadcastMode::ALL);
  assert(defaultRebroadcast(Role::CLIENT_MUTE) == RebroadcastMode::ALL);
  assert(defaultRebroadcast(Role::ROUTER) == RebroadcastMode::CORE_PORTNUMS_ONLY);
  assert(!relays(Role::CLIENT_MUTE, RebroadcastMode::ALL));
  assert(!relays(Role::CLIENT, RebroadcastMode::NONE));
  assert(relays(Role::CLIENT, RebroadcastMode::ALL));
  assert(relays(Role::ROUTER, RebroadcastMode::CORE_PORTNUMS_ONLY));

#ifdef CONFIG_MT_TAK_TRACKER
  static_assert(Role::TAK_TRACKER == 10u, "stock role value changed");
  assert(supportedRole(Role::TAK_TRACKER));
  assert(defaultRebroadcast(Role::TAK_TRACKER) == RebroadcastMode::ALL);
  // Stock TAK_TRACKER shares ordinary-client relay/duplicate behavior, while
  // allowing the explicit rebroadcast NONE mode.
  for (uint32_t mode : {0u, 2u, 3u, 4u, 5u})
    assert(valid(Role::TAK_TRACKER, mode));
  assert(relays(Role::TAK_TRACKER, RebroadcastMode::ALL));
  assert(relays(Role::TAK_TRACKER, RebroadcastMode::LOCAL_ONLY));
  assert(!relays(Role::TAK_TRACKER, RebroadcastMode::NONE));
#else
  assert(!supportedRole(10u));
  assert(defaultRebroadcast(10u) == kUnsupported);
  assert(!valid(10u, RebroadcastMode::ALL));
  assert(!relays(10u, RebroadcastMode::ALL));
#endif

  for (uint32_t role : {5u,6u,7u,8u,9u,11u,12u}) {
    assert(supportedRole(role));
    assert(defaultRebroadcast(role)==(role==Role::CLIENT_HIDDEN?RebroadcastMode::LOCAL_ONLY:RebroadcastMode::ALL));
  }
  for (uint32_t role : {3u,4u,13u,255u,UINT32_MAX}) {
    assert(!supportedRole(role));
    assert(defaultRebroadcast(role)==kUnsupported);
  }
  for (uint32_t mode : {1u, 6u, 255u, UINT32_MAX}) {
    assert(!supportedRebroadcast(mode));
    assert(!valid(Role::CLIENT, mode));
#ifdef CONFIG_MT_TAK_TRACKER
    assert(!valid(Role::TAK_TRACKER, mode));
#endif
  }
  assert(!valid(Role::ROUTER, RebroadcastMode::NONE));
  assert(!relays(Role::ROUTER, RebroadcastMode::NONE));
  std::puts("PASS bounded role/rebroadcast policy and conditional TAK_TRACKER role10");
}
