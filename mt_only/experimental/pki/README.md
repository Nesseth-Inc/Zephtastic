# Offline PKI compatibility experiment

**This directory is excluded from normal firmware. An opt-in M9 build retains
it only for inactive target link/resource checks. Private messaging remains
unsupported at runtime and must continue to return PKI_FAILED.** These tests do
not generate credentials, provision nodes, change identities, transmit or flash.

`PkiCrypto` is a stateless, bounded adapter for the pinned Meshtastic packet
cryptography. It accepts explicit caller-owned, already-clamped X25519 private
scalars and peer public keys. It retains neither keys nor plaintext. Monocypher
4.0.3 supplies X25519; an explicit backend supplies SHA256, HMAC-SHA256 and CCM.
The default test uses host OpenSSL. M9 adds a real PSA backend and bounded session
groundwork under `src/pki`, plus actual pinned-PSA host tests. Neither is called by
the running firmware; see [M9 groundwork](../../PKI-GROUNDWORK-M9.md).

The packet format matches firmware commit
`8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`:

- SHA256(X25519(private scalar, peer public key)) is the 32-byte AES key.
- AES-256-CCM uses a 13-byte nonce, an 8-byte tag and no additional authenticated
  data. The nonce contains little-endian packet ID, random extra nonce, source
  node ID and a trailing zero byte. The caller supplies the extra nonce.
- Ciphertext is followed by the tag and four-byte extra nonce. The adapter caps
  plaintext at 227 bytes so the resulting payload fits 239 bytes.
- The eight-byte ACK proof is truncated HMAC-SHA256 over `ack`, little-endian
  ACK source, ACK destination, request ID and exact encoded Routing bytes.

The adapter does not generate a nonce, check sender/destination policy, authenticate
an advertised peer key, bind a public key to a node ID or enforce ACK policy.
An eventual caller must do all of that. Failed operations leave caller output
unchanged and return failure with output length zero. Unauthenticated backend
plaintext is held in a temporary buffer and wiped before returning. Blank or
unclamped private scalars, zero shared secrets, short/oversized ciphertext,
insufficient output space and backend failures are rejected.

## Reproduce

On macOS with clang and Homebrew OpenSSL 3:

```sh
python3 mt_only/experimental/pki/test.py --output /tmp/zt-pki-differential
```

Use `--openssl PATH` to select another OpenSSL installation. The runner verifies
existing firmware/Crypto dependency locks and the two newly retained unchanged
`aes-ccm` files before compiling. It extracts ten exact complete CryptoEngine
functions, compiles the unchanged stock CCM implementation with the pinned
Meshtastic Crypto AES/Curve25519/SHA256 implementations, and compares that path
with Monocypher plus OpenSSL. Only framework class declarations, diagnostics and
hardware RNG inputs are substituted. RNG returns deterministic **public test
fixture** bytes. Upstream cryptographic decisions are not reimplemented as an
oracle.

Coverage includes 681 byte-exact encryption vectors over every supported payload
length and three extra nonces; stock-to-candidate and candidate-to-stock decrypt;
234 exact ACK-proof vectors; all 239 bytes of a maximum payload individually
mutated; source/request/direction binding; proof byte mutations; weak points;
unclamped and absent keys; input bounds; and injected backend failures. ASAN and
UBSAN cover the candidate, oracle and retained source. System OpenSSL itself is
not rebuilt with sanitizers. macOS leak detection is disabled. Tests print no
key or plaintext values. Results include the actual OpenSSL version, source
hashes, exact compile commands and extraction provenance.

## Integration blockers

| Capability | Current evidence | Still required |
|---|---|---|
| PKI encryption/decryption and ACK-proof format | Offline differential tests only | Target backend, board resource/timing checks and physical stock interoperability |
| Private credential lifecycle | Absent | Strong entropy, crash-safe private storage, backup/recovery/access policy and explicit provisioning semantics |
| Node identity | Existing ZT hardware-derived identity preserved | Decide identity migration: pinned stock NodeDB assigns node ID from CRC32(public key); signed first-contact bootstrap checks that binding |
| Peer-key discovery and trust | Absent | Key-bearing NodeInfo, validated key provenance, collision/change/downgrade handling and durable peer records |
| Companion API | Runtime explicitly rejects PKI | SecurityConfig, public-key export policy and private-message routing without public fallback |
| ACK authenticity | Experimental proof primitive only | Preserve exact Routing bytes, bind expected peer/request/direction and enforce proof before success or retry cancellation |
| RF integration | Absent | Header admission, encoding/decoding, signature interaction, queues and target recovery behavior |

No stock router, BLE, USB or user-interface parity follows from these primitive
tests. Keep this experiment outside release runtime until all integration gates
have an implemented and independently reviewed design.

## M8 ACK wire groundwork

`PkiAck` adds bounded proof attachment/extraction and pending-context verification.
It preserves the exact encoded Routing bytes, including unknown/reordered fields,
and returns only a proof verdict; successful delivery still requires a separate
Routing-result and peer-trust policy. It rejects malformed/duplicate proofs,
reserved tag zero and wrong-type field 4. The latter two are intentionally
stricter than the stock low-level extractor. It never establishes peer-key trust
or changes retry/routing state. No runtime file or capability is changed.

The same test command also compiles four unchanged stock `AckProof.cpp` functions
with the pinned nanopb sources. The real crypto oracle is reused; only framework
packet/context declarations and its single-thread lock are substituted. Tests
compare exact proof bytes, insertion/excision with unknown fields and nonminimal
encodings, malformed/duplicate proof handling, context/key binding, output
containment, and 20,000 deterministic parser mutations. These tests are reproducible
from the retained source tree without an external checkout or device.

See [the integration design](INTEGRATION-DESIGN.md) for the distinction between
pairwise encryption, peer-key binding, proof validity and delivery; the private
key lifecycle/identity migration decisions; and remaining authorization gates.
