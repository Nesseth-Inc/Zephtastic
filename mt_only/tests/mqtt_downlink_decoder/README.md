# M115 MQTT downlink decoder fixtures

`run.py` builds the bounded decoder without `PB_ENABLE_MALLOC` and separately
builds a stock-compatibility fixture with malloc enabled for nanopb's
pointer-backed `ServiceEnvelope`, matching upstream `DecodedServiceEnvelope`.
Both suites run with ASan/UBSan in default and short-enum modes.

The runner checks M115 baseline ancestry at `2d11cfb22c97017e4411cfd6ebe4c93f1d6e7cd6`,
the MQTT codec hash, the M114 source-lock hash, and every generated protobuf
header/nanopb source hash recorded by that lock. It compiles the current
production `Protobuf.cpp` descriptor bindings and prints that file's SHA-256 as
`EXECUTION_INPUT` evidence instead of pinning it to a historical source hash;
M116 legitimately adds `MapReport` there. The runner does not require a clean
worktree, so parallel scoped edits can proceed.

The stock reference is Meshtastic firmware revision
`8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. `MQTT.cpp` has SHA-256
`dd9005e52ffea560c805fd40ae2fd937f96c0f5cb76ccb01083ad42854590f06`;
`ServiceEnvelope.cpp` has SHA-256
`839bb6ead85a23ef5ae977075b3e5ba99bc2d26f65a6bc915844fe7ad7a8feeb`; its
header has SHA-256
`6826aa0f2c398dd7edcde7eb6d2f9e9fd1b6b5320f547f9d110ed9d9183b9895`. The
generated schema and nanopb 0.4.9.1 runtime are verified from M114's lock.

The fixtures verify stock descriptor behavior for reordered fields, unknown
field skipping, duplicate singular-field last-value handling, and last oneof
branch behavior. They also check truncation, varint overflow, unsupported wire
types, envelope/string bounds, nested static-array overflow, encrypted packet
preservation, and failure atomicity. They establish host decoder behavior only;
they do not exercise broker transport, packet admission, or RF dispatch.

Run from the M115 checkout root:

```sh
python3 -B mt_only/tests/mqtt_downlink_decoder/run.py
```
