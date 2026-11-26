// SPDX-License-Identifier: GPL-3.0-only
// Supported operating roles and stock rebroadcast modes.
// Bounded policy over a subset of the stock config.pb.h enum values:
//   roles: CLIENT=0, CLIENT_MUTE=1, ROUTER=2, ROUTER_LATE=11, CLIENT_BASE=12;
//   TRACKER=5, SENSOR=6, TAK=7, CLIENT_HIDDEN=8, LOST_AND_FOUND=9 and optional TAK_TRACKER=10.
//   rebroadcast: ALL=0, LOCAL_ONLY=2, KNOWN_ONLY=3, NONE=4, CORE_PORTNUMS_ONLY=5
// Deliberately out of scope: ALL_SKIP_DECODING=1 (stock schema limits it to
// the deprecated Repeater role, mapping to ALL on other roles); not all stock
// device roles are covered.
// Contract:
//   supportedRole/supportedRebroadcast: value is in the listed set.
//   valid: both values supported, and not (ROUTER, NONE) — the bounded helper
//   rejects this pair for ROUTER and ROUTER_LATE; Admin normalizes it to ALL.
//   defaultRebroadcast: ROUTER -> CORE_PORTNUMS_ONLY; CLIENT/CLIENT_MUTE/
//   CLIENT_BASE -> ALL. With CONFIG_MT_TAK_TRACKER, TAK_TRACKER -> ALL (the stock role
//   defaults do not assign rebroadcast_mode, so this is the product default).
//   relays: valid && role != CLIENT_MUTE && mode != NONE.
//   Unsupported input: predicates return false; defaultRebroadcast returns
//   kUnsupported.

#pragma once

#include <cstdint>

namespace mt {
namespace DeviceRolePolicy {

namespace Role {
constexpr uint32_t CLIENT = 0u;
constexpr uint32_t CLIENT_MUTE = 1u;
constexpr uint32_t ROUTER = 2u;
constexpr uint32_t TRACKER = 5u, SENSOR = 6u, TAK = 7u, CLIENT_HIDDEN = 8u, LOST_AND_FOUND = 9u;
constexpr uint32_t ROUTER_LATE = 11u;
constexpr uint32_t CLIENT_BASE = 12u;
#ifdef CONFIG_MT_TAK_TRACKER
constexpr uint32_t TAK_TRACKER = 10u;
#endif
}  // namespace Role

namespace RebroadcastMode {
constexpr uint32_t ALL = 0u;
constexpr uint32_t LOCAL_ONLY = 2u;
constexpr uint32_t KNOWN_ONLY = 3u;
constexpr uint32_t NONE = 4u;
constexpr uint32_t CORE_PORTNUMS_ONLY = 5u;
// ALL_SKIP_DECODING = 1 is intentionally excluded (Repeater-only in stock).
}  // namespace RebroadcastMode

constexpr uint32_t kUnsupported = 0xFFFFFFFFu;

inline bool supportedRole(uint32_t role) {
#ifdef CONFIG_MT_T096_ROUTER_ONLY
  return role == Role::ROUTER || role == Role::ROUTER_LATE;
#else
  if (role == Role::CLIENT || role == Role::CLIENT_MUTE || role == Role::ROUTER ||
      role == Role::ROUTER_LATE || role == Role::CLIENT_BASE || role == Role::TRACKER ||
      role == Role::SENSOR || role == Role::TAK || role == Role::CLIENT_HIDDEN || role == Role::LOST_AND_FOUND)
    return true;
#ifdef CONFIG_MT_TAK_TRACKER
  return role == Role::TAK_TRACKER;
#else
  return false;
#endif
#endif
}

inline bool supportedRebroadcast(uint32_t mode) {
  return mode == RebroadcastMode::ALL || mode == RebroadcastMode::LOCAL_ONLY ||
         mode == RebroadcastMode::KNOWN_ONLY || mode == RebroadcastMode::NONE ||
         mode == RebroadcastMode::CORE_PORTNUMS_ONLY;
}

inline bool valid(uint32_t role, uint32_t mode) {
  if (!supportedRole(role) || !supportedRebroadcast(mode)) {
    return false;
  }
  return !((role == Role::ROUTER || role == Role::ROUTER_LATE) && mode == RebroadcastMode::NONE);
}

// A known persisted role is not corrupt merely because this firmware omits
// its optional producer. Keep it readable without enabling that operation.
inline bool storedValid(uint32_t role,uint32_t mode) {
  return (role==0u||role==1u||role==2u||role==5u||role==6u||role==7u||role==8u||role==9u||role==10u||role==Role::ROUTER_LATE||role==Role::CLIENT_BASE)&&
    supportedRebroadcast(mode)&&!((role==2u||role==Role::ROUTER_LATE)&&mode==RebroadcastMode::NONE);
}

inline uint32_t defaultRebroadcast(uint32_t role) {
  if (role == Role::CLIENT_HIDDEN)return RebroadcastMode::LOCAL_ONLY;
  if (role == Role::ROUTER) {
    return RebroadcastMode::CORE_PORTNUMS_ONLY;
  }
  if (role == Role::CLIENT || role == Role::CLIENT_MUTE || role == Role::TRACKER || role == Role::SENSOR || role == Role::TAK || role == Role::LOST_AND_FOUND) {
    return RebroadcastMode::ALL;
  }
  if (role == Role::CLIENT_BASE) {
    return RebroadcastMode::ALL;
  }
  if (role == Role::ROUTER_LATE) {
    return RebroadcastMode::ALL;
  }
#ifdef CONFIG_MT_TAK_TRACKER
  if (role == Role::TAK_TRACKER) {
    return RebroadcastMode::ALL;
  }
#endif
  return kUnsupported;
}

inline bool relays(uint32_t role, uint32_t mode) {
  return valid(role, mode) && role != Role::CLIENT_MUTE &&
         mode != RebroadcastMode::NONE;
}

}  // namespace DeviceRolePolicy
}  // namespace mt
