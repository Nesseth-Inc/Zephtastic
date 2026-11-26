// m96 power-helper assertions (proposal only).
// Scope: pure software behavior of the existing ManualLoRa helpers.
// - -9 (T096) / +2 (XIAO) are legacy per-board defaults, not board caps.
// - [-9,22] is the helper's SX1262 chip-API boundary, not RF output,
//   antenna, or board performance evidence.
// - Nothing here claims any value is legal or recommended operating power.
// - No hardware/radio involved; no performance claims.
static bool m96_power_layer_assertions() {
  bool ok = true;
#define M96_CHECK(cond) do { if (!(cond)) ok = false; } while (0)

  // (a) regionalPower: regional software selection, US table limit 30.
  M96_CHECK(ManualLoRa::regionalPower(0, 30, false) == 30);   // request 0 -> limit
  M96_CHECK(ManualLoRa::regionalPower(31, 30, false) == 30);  // unlicensed over-limit -> limit
  M96_CHECK(ManualLoRa::regionalPower(31, 30, true) == 31);   // licensed passes through
  M96_CHECK(ManualLoRa::regionalPower(0, 0, false) == 17);    // limit 0 + request 0 -> fallback 17
  M96_CHECK(ManualLoRa::regionalPower(-9, 30, false) == -9);  // negative request remains negative
  M96_CHECK(ManualLoRa::regionalPower(-128, 30, false) == -128); // negatives pass selection through

  // (b) effectivePower, powerSpecified=false: legacy per-board defaults
  // (T096 -9 / XIAO +2). Defaults, not caps.
  {
    RadioSettings::Request r; // powerSpecified=false by default
    M96_CHECK(RadioSettings::effectivePower(r, /*t096=*/true) == -9);
    M96_CHECK(RadioSettings::effectivePower(r, /*t096=*/false) == 2);
  }

  // (c) effectivePower, powerSpecified=true: first selected against US
  // limit 30 as unlicensed, then clamped to SX1262 range [-9,22]
  // (chip-API boundary, not RF output evidence).
  auto eff = [](int p) {
    RadioSettings::Request r;
    r.powerSpecified = true;
    r.power = p;
    return RadioSettings::effectivePower(r, /*t096=*/true);
  };
  M96_CHECK(eff(-128) == -9); // below chip range -> lower bound
  M96_CHECK(eff(-10) == -9);  // just below bound -> clamped up
  M96_CHECK(eff(-9) == -9);   // at lower bound
  M96_CHECK(eff(8) == 8);     // in range -> passes through
  M96_CHECK(eff(2) == 2);     // XIAO default value, in range, passes through
  M96_CHECK(eff(22) == 22);   // at upper bound
  M96_CHECK(eff(30) == 22);   // passes selection at 30, clamps to 22
  M96_CHECK(eff(31) == 22);   // unlicensed 31 -> 30, then clamps to 22
  M96_CHECK(eff(0) == 22);    // specified 0 -> 30 via selection, clamps to 22

  // Layer distinction: when powerSpecified=true, the t096 flag no longer
  // matters; -9/+2 are defaults only, and the clamp is the chip boundary.
  {
    RadioSettings::Request a, b;
    a.powerSpecified = b.powerSpecified = true;
    a.power = b.power = 8;
    M96_CHECK(RadioSettings::effectivePower(a, true) == RadioSettings::effectivePower(b, false));
  }

#undef M96_CHECK
  return ok;
}
