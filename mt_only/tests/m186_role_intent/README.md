# M186 role-intent native regression fixture

This fixture exercises the production `mt::admin::RemoteDeviceTransaction`, the actual Config/Module/Completion/Notification/MQTT dual-slot journals, and the existing Monocypher BLAKE2b implementation against a bounded in-memory `SettingsBackend`. It does not copy the transaction algorithm into the fixture. The test backend adds deterministic read/write faults around the real journal calls.

`run.py` takes an explicit source root, the full 40-character expected source commit, and a new output directory outside that source root. For a Git checkout it refuses a mismatched HEAD or dirty worktree. For an exported source tree, it requires a sibling `SOURCE-MANIFEST.json`, checks its full source revision, rejects duplicate or unsafe normalized paths, and verifies every listed file against its SHA-256. It also verifies the manifest identity and all listed hashes again at the end. The runner compiles the production protobuf descriptor aggregator and nanopb sources, runs default and `-fshort-enums` variants with ASan/UBSan, records source and test artifact SHA-256 pins plus stage-specific failures, and checks the selected source inputs again at the end. It does not configure a target build, use hardware, or access a network.

Example invocation shape (supply the exact source head and a fresh external output path):

```sh
python3 run.py --source /path/to/source --expected-head <40-hex-commit> --output /tmp/role-intent-host
```

For an exported source tree, place `SOURCE-MANIFEST.json` beside the source directory, for example `/path/to/SOURCE-MANIFEST.json` beside `/path/to/source`. The manifest uses `source_revision` and a `files` array of `{ "path": "...", "public_sha256": "..." }` entries.

The fixture checks twelve recovery scenarios.

1. Pending intent visibility before typed saves and successful verification of all five sections plus completion marker.
2. Commit-after-error at the Pending write, each of five section writes, and Completed marker, followed by fresh-owner recovery.
3. Per-section typed-journal readback failure followed by fresh-owner recovery.
4. Pending intent readback failure without any section callback, followed by recovery.
5. Explicit callback failure, storage-pause rejection with no writes, and no candidate mutation before the pending intent is durable.
6. Stale third-value hold, wrong-domain Pending hold, and historical Completed behavior without replay.
7. Corrupt intent, oversized intent, backend I/O error, and generation exhaustion handling.
8. Consent-only and managed-LoRa-only typed MQTT save restrictions, including credential/TLS preservation and unauthorized-delta rejection.
9. Transaction-level channel-consent binding and rejection of a mixed consent plus LoRa policy candidate.
10. V22 and V23 pending-intent recovery before and after the core journal write, original bytes and BLAKE2b digests, V23 primary and secondary AEAD settings, historical completion metadata, and a fresh current transaction after completion.
11. Refusal to overwrite a divergent V24 sender filter or accept a changed V22/V23 source digest.
12. Refusal to down-project primary or secondary V23 AEAD state into V22, plus corrupt V22 core CRC, intent CRC, and reserved-byte rejection.

The fixture exercises the production transaction and typed journals against synthetic storage. Physical NVS endurance and field RF behavior require hardware qualification.
