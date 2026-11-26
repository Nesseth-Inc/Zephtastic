# Computed keyed hash-zero regressions

This focused native fixture must be run against the exact reviewed source head supplied by the validation owner. It uses actual production `Config`, `PrimaryChannel`, `MtCodec`, `Service`, `Companion`, `Endpoint`, and `RoutingCore` headers with generated nanopb and actual codec/signature implementation. Only the radio and private-runtime classification result are synthetic. No radio, crypto identity, durable PKI authority, PSA authentication-tag rejection, hardware, or network claim is made by these cases.

The test derives a 16-byte key whose XOR with the actual `LongFast` name yields hash zero, then checks Config encode/decode and effective selection. Other cases cover default codec refusal versus explicit configured encrypted-unicast decode, telemetry query encoding, separation from licensed plaintext/unconfigured modes, default signature-verifier refusal versus its explicit Service-resolved context admission, Service PKI-first outcome handling and USB/BLE shared-fanout proof, explicit zero expected-hash retry matching, and RoutingCore's configured zero hash plus channel-context and ambiguity-marker gates.

The ambiguity case supplies the actual `channelAmbiguous` rejection metadata to `RoutingCore`; it does not synthesize a dual-success AES collision through `Service::decodeChannel`. The PKI test probe supplies the classification enum and verifies Service sequencing only; separate pinned PSA backend tests remain necessary to qualify how real cryptographic failure becomes `Unauthenticated` versus `Hold`.

Root-owned execution command (run after source/test review, with a fresh output directory outside the checkout):

```sh
python3 mt_only/tests/m186_hash_zero/run.py \
  --source /path/to/zt \
  --expected-head <reviewed-source-commit> \
  --output /tmp/m186-hash-zero-host-r1
```

The runner accepts either a clean Git checkout or an exported source directory with sibling `SOURCE-MANIFEST.json`. It validates the exact revision and all manifest-listed SHA-256 entries, including that every selected fixture input is listed. It refuses a different source revision, dirty Git source, or an existing/in-tree output path. It hashes and freezes source and test inputs, generates the pinned protobuf include tree, compiles with AddressSanitizer and UndefinedBehaviorSanitizer, records toolchain/commands/case output and failures, and confirms source/test inputs did not change. `cases.cpp` defines the Zephyr/Linux `EKEYREJECTED` extension as errno 129 only when the host C library omits it; assertions remain unchanged. This authoring task did not execute the runner or any compiler/test command; it only prepared the fixture for the combined validation owner.
