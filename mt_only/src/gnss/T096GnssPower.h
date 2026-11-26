// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 ZephCore contributors
// T096GnssPower.h: board-specific (T096) passive GNSS power sequencing
// adapter. Proposal only; not a new GNSS framework. Pure C++17, no Zephyr
// includes. Donor: zephcore/adapters/gps/gps_power.cpp (SHA256
// 6dabdd42aa5de72cacc5b6b413f99c56e62e5912f088bf9a34129914d56a454e;
// committed Git blob076d2919bd8c1e22af7296dd2e5b280bef5ca6c1).
// Ops callbacks are idempotent/state-aware, owned by the Root driver; no
// callback proves timing/readiness/freshness. Stateless, command-free,
// caller-serialized; no timing/readiness/electrical behavior established.
// Does not claim a fitted or working receiver or rail. Logical reset
// booleans only; active-low target mapping (VGNSS P0.06, RESETN P1.14)
// lives in the Root-owned driver callbacks.

#ifndef ZT_GNSS_T096_POWER_H
#define ZT_GNSS_T096_POWER_H

namespace zt::gnss::t096 {

// Validation failures, reported before any callback is invoked. Carried in a
// dedicated discriminant (Result::validation) so they never occupy the
// callback error-code space used by Result::op_error / rollback_error.
enum class Validation {
  kOk = 0,
  kInvalidState = 1,    // some input state unknown
  kMissingCallback = 2, // some Ops callback is null
};

// Per-channel snapshot: a value plus whether that value is known.
struct State {
  bool rail_known = false;
  bool rail_enabled = false;
  bool reset_known = false;
  bool reset_asserted = false;  // logical; active-low in Root callbacks
  bool uart_known = false;
  bool uart_enabled = false;
  bool allKnown() const { return rail_known && reset_known && uart_known; }
};

struct Ops {
  void* context;
  int (*setRailEnabled)(void* context, bool enabled);
  int (*setResetAsserted)(void* context, bool asserted);
  int (*setPassiveUart)(void* context, bool enabled);
};

struct Result {
  Validation validation = Validation::kOk;  // non-kOk: validation failed, no callback invoked
  int op_error = 0;      // original op error; 0 on success; 0 on validation failure
  int rollback_error = 0; // first nonzero best-effort restore error; 0 if none
  State final;           // a channel is known only if its value is established
};

// start: rail enabled -> reset deasserted -> passive UART enabled.
// stop:  passive UART disabled -> reset asserted -> rail disabled.
// Steps already at target are skipped (no callback). On a callback error,
// op_error keeps the original code and every attempted step is best-effort
// restored to the supplied pre-state in reverse order; rollback_error is
// reported separately and any step whose final value cannot be known is marked
// unknown. An already-correct rail is never touched, so a failed start never
// unconditionally disables an already-enabled rail.
inline Result run(const Ops& ops, const State& pre, bool start) {
  Result r;
  r.final = pre;
  if (!pre.allKnown()) { r.validation = Validation::kInvalidState; return r; }
  if (ops.setRailEnabled == nullptr || ops.setResetAsserted == nullptr ||
      ops.setPassiveUart == nullptr) { r.validation = Validation::kMissingCallback; return r; }
  struct S { bool pv, target; int (*cb)(void*, bool); bool* fv, *fk; };
  State& f = r.final;
  S st[3];
  int n = 0;
  if (start) {
    st[n++] = {pre.rail_enabled, true, ops.setRailEnabled, &f.rail_enabled, &f.rail_known};
    st[n++] = {pre.reset_asserted, false, ops.setResetAsserted, &f.reset_asserted, &f.reset_known};
    st[n++] = {pre.uart_enabled, true, ops.setPassiveUart, &f.uart_enabled, &f.uart_known};
  } else {
    st[n++] = {pre.uart_enabled, false, ops.setPassiveUart, &f.uart_enabled, &f.uart_known};
    st[n++] = {pre.reset_asserted, true, ops.setResetAsserted, &f.reset_asserted, &f.reset_known};
    st[n++] = {pre.rail_enabled, false, ops.setRailEnabled, &f.rail_enabled, &f.rail_known};
  }
  bool att[3] = {false, false, false};
  for (int i = 0; i < n; ++i) {
    const S& s = st[i];
    if (s.pv == s.target) { *s.fv = s.pv; *s.fk = true; continue; }
    att[i] = true;
    int ret = s.cb(ops.context, s.target);
    if (ret == 0) { *s.fv = s.target; *s.fk = true; continue; }
    r.op_error = ret;  // preserve the original operation error
    for (int j = i; j >= 0; --j) {  // restore attempted steps, reverse order
      if (!att[j]) continue;
      const S& t = st[j];
      int rret = t.cb(ops.context, t.pv);
      if (rret == 0) { *t.fv = t.pv; *t.fk = true; }
      else { if (!r.rollback_error) r.rollback_error = rret; *t.fv = t.pv; *t.fk = false; }
    }
    return r;
  }
  return r;  // all steps succeeded or were skipped; all final values known
}

inline Result start(const Ops& ops, const State& pre) {
  return run(ops, pre, true);
}
inline Result stop(const Ops& ops, const State& pre) {
  return run(ops, pre, false);
}
}  // namespace zt::gnss::t096
#endif  // ZT_GNSS_T096_POWER_H
