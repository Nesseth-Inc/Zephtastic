# M60 measured TX airtime checks

Run `mt_only/scripts/test-airtime-quality.py --fixture accounting --output <external-directory>`.
The other fixture selectors are `meter-quality`, `window-math`, `actual-responder` and
`local-telemetry`; all support `--short-enums`. The standard offline catalog
registers all ten combinations and the process-cleanup controls. Stock client cases require Meshtastic 2.7.11
with the exact client and cache implementation hashes checked by the fixture.

These tests exercise the production Service, Endpoint, Companion and encoder.
The adapter binds a controlled 64-bit observation clock and reads the production
meter; it never supplies an airtime result. The two wire bridges use actual
locally generated responses, covering both board models, USB/BLE owners,
cold/warm caches and seven measurement-quality states. They open no device port.

Known clean completions credit their cached estimate exactly once. Known idle
reports zero. Missing clocks and uncertain completion suppress the optional
field for a full hour after trustworthy terminal time, or after the first later
trustworthy observation. This conservative failure policy is stricter than
stock fault accounting. Measurements use the fixed one-hour denominator and
absolute minute buckets. Stock client cache merging may retain an older field
when the new producer message omits it; wire presence is asserted separately.

The driver snapshots source, fixture and resolved compiler/interpreter hashes
before execution and checks them afterward. This is an offline reproducibility
receipt; it does not authorize an image installation or demonstrate target RF,
RTOS timing or physical shutdown.

The window-math fixture preserves all 279 original counter assertions. The
production availability type is allocation-free and works with Zephyr minimal
C++; its math uses fixed-width integer limits without `<limits>` or `<optional>`.
