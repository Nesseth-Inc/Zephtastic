# M9 private messaging groundwork tests

These tests run on the host only. All key material is fixed public synthetic test
input. No device, BLE, serial, radio, live settings, credential store or network
is accessed. Firmware's normal PKI/private-message rejection is unchanged.

## Reproduce

Use the dependency checkout pinned in `mt_only/dependency-lock.json`:
TF-PSA-Crypto `765af96a20a2b408474dcd950372ba9af4d26b62` (1.1.1). The existing
dependency fetch workflow supplies it; this runner does not fetch anything.
On macOS with clang and CMake:

```sh
python3 mt_only/tests/pki/run.py \
  --tf-psa /path/to/modules/crypto/tf-psa-crypto \
  --output /tmp/zt-pki-psa-tests
```

Use `--ninja /path/to/ninja` if it is not on PATH, or let the runner use Make.
The runner rejects a mismatched or modified dependency checkout, builds the real
pinned PSA library under ASAN/UBSAN and then runs the unchanged pinned stock
crypto/ACK oracle against the PSA-backed candidate. The host config enables only
SHA256, HMAC and AES-CCM, with no persistent key storage. Its external RNG always
returns insufficient entropy; these tests cannot generate new credentials.

The existing OpenSSL comparison remains available:

```sh
python3 mt_only/experimental/pki/test.py --output /tmp/zt-pki-openssl-tests
```

PSA fault tests inject failures at the API boundary while using the real pinned
implementation on successful paths. They verify volatile-only key attributes,
output staging, import-after-allocation failure, shortened/failing hash/MAC/AEAD
results, decryption that scribbles before failing, slot cleanup, and permanent
poison after uncertain destruction. A regression forces reuse of the exact
volatile ID after an injected post-destruction error and verifies that close and
destruction do not destroy the new owner's key. Component wipe calls are
instrumented to call the actual Monocypher wipe and check zeroed bytes; nested
PSA/Monocypher internal wipes are not all instrumented.

Session tests cover two synthetic peers, frame bytes and endpoint fields on
retry, full queue admission, key generation revocation, incorrect provider keys,
reservation burns on entropy/provider/crypto failure, identical-key reimport
across generations, each byte of a maximum ciphertext tampered before valid
receipt, replay rejection, and fail-closed capacity limits. They are state/crypto
tests, not transport tests or proof of stock application delivery.

`RESULTS.json` records dependency/library/config hashes and source hashes.
`commands-private.json` and compiler logs include local paths and should remain
private or be sanitized before publication. No key or plaintext values are logged.
