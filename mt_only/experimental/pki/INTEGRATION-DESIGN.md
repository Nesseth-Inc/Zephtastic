# PKI integration design, M8 groundwork

M9 follow-up: [target/backend/session groundwork](../../PKI-GROUNDWORK-M9.md)
now provides actual PSA host tests and inactive target linking. The runtime
identity/trust/provisioning gates below remain unimplemented application flows.

This is an **unimplemented runtime design**. The accompanying `PkiAck` and
`PkiCrypto` code is host-tested and excluded from normal firmware; M9 has a separate
opt-in inactive target-link hook. No credential is
created or stored, no device identity changes, and runtime private messaging
continues to return PKI_FAILED. Generic firmware must never contain a fixture
private key. Continued parity work does not override the existing prohibition
on creating persistent credentials.

## What the new adapter establishes

`PkiAck` appends the pinned stock field-4 eight-byte proof, extracts that field
wherever it occurs, and verifies the remaining encoded Routing bytes without
re-encoding them. It preserves unknown fields, ordering, and accepted nonminimal
protobuf encodings. An active pending record and matching peer, local node,
request ID and decoded Routing envelope are required before any crypto operation.
The caller explicitly supplies the authoritative public key of the expected peer;
passing the key advertised by the incoming packet would defeat this protection.

`ValidProof` means the exact Routing bytes and direction/request tuple match the
pairwise key. It is **not** a success ACK, evidence that a human read the message,
a publicly verifiable signature, or a delivery claim. Either endpoint knows the
pairwise secret. The caller must decode the authenticated Routing bytes, decide
whether they report success or a NAK, and apply pending expiry and replay rules.
The adapter does not consume the pending record or change retry/routing state.
A repeated valid proof remains cryptographically valid until the caller retires
that record. It cannot prove that the caller supplied the right peer key.

Pinned stock `AckProof.h` explicitly keeps ACK proof enforcement advisory and
states that header-only implicit ACKs can cancel retransmissions independently
of proof verification. M8 does not change that runtime policy. Missing, invalid
or malformed proofs never produce `ValidProof` in this experiment; the result
must not be turned into an invented stock delivery guarantee.

The parser deliberately rejects reserved field zero and field 4 with any wire
type other than a length-delimited eight-byte proof. Stock's low-level extractor
skips those cases like unknown fields. Tests capture these stricter admissions;
no claim of identical malformed-input behavior is made. Duplicate proofs and
truncated/oversized encodings fail. Output remains unchanged on failure, with
output length zero. A Routing payload including proof is capped at 233 bytes.

## Credentials and identity: required decisions before implementation

1. **Explicit provisioning scope.** Obtain authorization before creating any
   persistent private key on either designated radio. State whether this creates
   a new Meshtastic identity and what the owner's phone, saved contacts and peers
   will observe. Never silently copy another radio's key or installed identity.
2. **Preserve current identity by default.** Current ZT node numbers are derived
   from hardware. Pinned stock `NodeDB.cpp` assigns a key-bearing node number
   using `crc32Buffer(public_key, 32)`, and `Router.cpp` first-contact NodeInfo
   verification checks that binding. Simply attaching a new key to the old ZT
   node number is not equivalent to stock bootstrap. The eventual migration
   needs a deliberate new identity, owner-visible old/new mapping, backups, and
   a recovery choice. Do not brute-force keys to retain a hardware-derived ID.
3. **Fail-closed entropy.** Use an independently reviewed board CSPRNG path with
   startup/readiness and failure handling. No clock, serial number, node ID,
   deterministic fixture, or fallback PRNG may supply a private key or CCM extra
   nonce. Packet-ID/extra-nonce reuse across resets, retry and queue replay needs
   an explicit prevention strategy; the stateless primitive does not provide it.
4. **One transaction for the identity tuple.** Persist the private scalar,
   derived public key, selected node number, schema/algorithm identifiers and
   generation as one validated record. A candidate record must be readable and
   self-consistent before activation. Use versioned records/commit markers with
   interruption tests so a reset cannot publish a public key without its matching
   private scalar. Never regenerate an identity automatically after corruption;
   remain radio-disarmed and report an actionable recovery state.
5. **Storage threat model.** Ordinary NVS plus a checksum detects accidental
   corruption, not a malicious rewrite or physical flash extraction. Board flash
   encryption/secure storage must be evaluated separately. Do not claim private
   keys are hardware protected simply because they are in NVS or behind BLE.
   Wipe working buffers, avoid key/derived-secret logs and crash dumps, and keep
   key material out of generic source, public artifacts and diagnostic receipts.
6. **Backup, import and reset policy.** Decide whether key export is supported,
   the authorized local interface and user-presence requirement, and how backups
   are protected. Public-key readback is distinct from private-key export.
   Rotation/import/factory reset must be separate explicit operations. Existing
   identity/config backups remain private. A firmware update must not reset keys,
   silently enable TX, or overwrite the owner's LoRa region/power.

## Peer-key binding is separate from encryption

X25519 plus CCM authenticates possession of the peer key that the caller supplied;
it does not tell us whose key it is. Unverified NodeInfo or a channel-key-protected
advertisement can plant a key. Such a key must not become authoritative merely
because a decrypt or ACK proof validates against itself.

Design peer records with explicit provenance: unverified candidate, signature
verified with the expected key/node binding, and owner-confirmed contact. Record
key changes, node-number collisions, reset/downgrade and expiry separately.
Key-bearing first-contact NodeInfo needs the pinned signing/bootstrap behavior;
CRC32 alone is a short binding check, not proof of identity or collision resistance.
Do not overwrite a pinned key through a received advertisement or a generic
companion owner/channel update. Any trust-on-first-use mode must be described as
such and must not silently downgrade an established key. Durable peer records
need interruption, deletion, key-change and stale-cache tests.

## Required runtime and test gates

| Area | Required work before advertising support |
|---|---|
| Target cryptography | SHA256/HMAC/CCM backend on each MCU; volatile key cleanup and injected failures; RAM/stack/timing measurements; keep constant-time comparison |
| Identity lifecycle | Authorized provisioning, entropy, atomic persistence, import/rotation/reset/recovery tests and explicit migration |
| Peer provenance | NodeInfo signature/bootstrap, confirmed-key policy, collision/change handling, persistent peer records |
| Companion | Truthful SecurityConfig and NodeInfo, public/private export rules, no public-channel fallback for a failed private send |
| Radio endpoint | PKI header/recipient admission, encoded Data bounds, sender binding, queue/retry nonce semantics and failed-decrypt containment |
| Receipt semantics | Exact Routing-byte MAC input, cheap pending gates, separate valid proof versus successful Routing result, explicit replay/expiry/implicit-ACK policy |
| Verification | Host differential and fault cases, both target builds/resource checks, independent review, bounded physical stock-node comparison under explicit device/RF scope |

Two ZT peers exercising the same implementation do not independently establish
stock interoperability. Until those gates are implemented and reviewed, keep
PKI capabilities absent and private sends rejected. No new BLE pairing or
weaker security is needed for these offline tests.

## Pinned evidence

All protocol comparisons use firmware commit
`8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`, not an unspecified latest release.

- `reference/AckProof.cpp` and `.h`: retained unchanged; hashes/URLs in the local
  source lock. The runner extracts all four mechanism functions unchanged.
- `zephcore/bench/upstream-differential/source/CryptoEngine.cpp`: existing locked
  primitive oracle; ten complete functions compiled unchanged.
- [NodeDB identity assignment](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/mesh/NodeDB.cpp#L537)
  and [Router first-contact binding](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/mesh/Router.cpp#L714).
- [ReliableRouter proof policy](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/mesh/ReliableRouter.cpp#L221)
  and retained `reference/AckProof.h`: proof advisory and implicit-ACK limits.
