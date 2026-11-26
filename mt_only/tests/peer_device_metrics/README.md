# Volatile peer device-metrics cache fixture

This fixture includes the actual `PeerDeviceMetrics.h`, `NodeCache.h` and
`NodeJournal.h`; it creates no helper or cache substitute. A small nanopb-shaped
struct supplies the five exact `has_*`/value pairs to the production templates.
It tests holder/cache/journal behavior rather than protobuf encoding, service
admission, stock-client interoperability or physical RF behavior.

The twelve groups cover:

- Measured host holder/entry/cache/journal layout and the unchanged durable
  92-byte entry / 1492-byte record. Expected host sizes are 21/120/1924/1948;
  the executable prints its actual measurements before checking them.
- Each of the five optional flags independently, known zero, full uint32
  battery and uptime, voltage `FLT_MAX`, fractional values and signed `-0.0f`.
- Atomic rejection of empty snapshots, NaN, infinities, negative floats and
  utilization above100. Absent numeric garbage is ignored. Voltage accepts
  finite0..FLT_MAX; each utilization accepts0..100; uint32 fields have no
  additional percentage limit. A valid whole snapshot clears absent fields.
- Observations allocate nothing, never change durable identity bytes or other
  heard metadata, preserve the existing dirty state and cause no backend I/O.
  Unknown IDs, empty holders, ignored peers, placeholders and frozen caches
  refuse without mutation.
- Same-peer identity refresh preserves the metrics holder; eviction/reuse,
  removal-tail reuse, placeholder creation and journal reload clear it.
  Shifted entries keep their own complete metadata.
- Local policy transaction candidate copies preserve live metrics on success;
  an uncertain commit freezes the unchanged prior live snapshot. Duplicate
  USB/BLE-style fanout remains idempotent.
- Identical durable histories produce identical complete two-slot journal
  bytes with and without metric observations. Reload loses all volatile
  metrics and last-heard metadata while retaining durable identities/policy.

`run.py` is inert on import and requires an explicit fresh output directory
outside the source worktree. It compiles default and `-fshort-enums` variants
with ASAN+UBSAN, then runs each bounded executable. The existing reviewed
`airtime_quality/bounded_process.py` owns and reaps each compiler/test process
group. Inputs and compiler are hashed before/after, each command/log and actual
size/count result is recorded, and output creation is exclusive. No network,
SSH, hardware or RF operation is present.

After the parent lands the production helper/integration and authorizes the
actual local run:

```
python3 -B zt-m66/mt_only/tests/peer_device_metrics/run.py \
  --output /absolute/fresh/output/directory
```

Preparation alone is not a passing test result. The helper and integration
must exist, the authorized runner must finish both variants, and its actual
`SUMMARY.json` must report PASS with clean owned-process closure and unchanged
input pins before any qualification claim.

The companion integration admits only canonical, matched Public DeviceMetrics
query replies for an already known, nonignored, nonplaceholder peer. Unmatched
or unsolicited telemetry never populates this cache. The whole latest snapshot
is exported through the existing cached NodeInfo encoder on a fresh handshake;
no new immediate node event is added. Stock Python merges sparse forwarded
telemetry dictionaries, so that path alone cannot prove absent-field clearing.
`export_run.py` covers actual Service/Endpoint/Companion/nanopb admission, all31
nonempty presence combinations and fresh NodeInfo exports on both board models
and owners. It also covers sparse replacement, malformed/wrong-correlation
rejection, unknown/ignored/placeholder peers, duplicate/consumed approvals and
shared-owner disconnect behavior. These deterministic fixtures do not establish
actual BLE, phone or physical RF interoperability.
