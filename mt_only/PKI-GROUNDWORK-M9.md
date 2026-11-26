# M9 target crypto and private-session groundwork

**Normal firmware still rejects PKI/private messages and advertises no PKC
capability.** The opt-in `MT_PKI_GROUNDWORK` build compiles and retains the new
code for target ABI/resource checks, but nothing in the application calls it.
No credential, identity change, radio operation or PSA initialization occurs at
startup through this hook. It contains no synthetic key or default key provider.

## What is implemented

- A real PSA SHA256/HMAC/AES-256-CCM backend using existing pinned dependencies.
  CCM uses the stock 13-byte nonce, eight-byte tag and no AAD. Keys are imported
  as volatile, with one operation's usage only. Caller output is published only
  after operation success, exact output length and successful immediate key
  destruction. Temporary key/plaintext/digest buffers are wiped.
- Permanent backend poison on initialization or uncertain cleanup failure.
  An uncertain volatile ID is never destroyed again later: its numeric slot may
  already belong to Bluetooth or another caller. `close()` reports unresolved
  cleanup; it cannot restore health. Eventual runtime integration must stop PKI
  and surface this fault, without resetting the shared PSA subsystem behind
  other users. No automatic recovery or device reset is supplied here.
- A callback-only private-key provider contract, with opaque slot/generation
  references and cheap active-generation checks. The pointer borrowed inside
  the callback cannot escape; bounded operation-local scalar copies are wiped.
  The session verifies the scalar's clamping and derived public key on every use.
- An immutable single-peer session with four pending ciphertext frames, a
  32-ID send budget and 32 accepted receive IDs. Successful retries return the
  same cached ciphertext, source, destination and ID, without keys or entropy.
  Retirement frees a pending slot but never recycles its nonce reservation.
  Replay entries never evict; capacity exhaustion fails closed. `close()` is
  terminal for that object. Copying/moving sessions is disabled.

## Caller contracts and remaining blockers

The supplied local node number is trusted caller input, not derived from or
validated against the provider's identity metadata. The peer key must already
be authoritatively bound to the intended peer. Neither agreement on a symmetric
key nor a valid CCM/ACK proof establishes that provenance. No peer discovery,
NodeInfo signature bootstrap, caller authorization or identity migration occurs
in the session. All calls must be serialized with provider mutations; providers
must not reenter or retain an operation callback.

The nonce authority must irrevocably reserve each packet ID for the **stable
local public-key/local-node domain**, across sessions, restarts and reimports of
the same key under a different generation. A KeyRef generation is a locator, not
a cryptographic nonce domain. The explicit public key is included in the API so
an authority can enforce this distinction. The authority must also provide an
unpredictable extra nonce from a reviewed CSPRNG. There is no runtime authority
implementation, weak fallback, automatic key creation or ID recycling. Within
the object, an accepted send attempt burns its ID before calling the authority;
entropy, provider and crypto failures do not free it. Invalid input/full-queue
attempts do not reserve an ID.

Receive IDs are recorded only after authenticated decryption. Malformed,
wrong-peer and failed-authentication inputs cannot poison the replay window.
The bound is explicit and conservative; cross-session replay protection remains
future caller work. `receive()` returns authenticated encoded Data bytes, without
interpreting app port, payload semantics, signed NodeInfo or delivery status.
Queue scheduling, ACK policy, retries on radio, USB/BLE, persistence and RF gates
remain outside this layer. Session crypto success is not a delivery receipt.

Persistent identity provisioning and migration remain unauthorized and absent
from normal application flows. In particular, the current hardware-derived node
number is preserved; pinned stock first-contact identity expects a public-key
CRC binding. The identity journal groundwork is separately reviewed and must
remain unwired until provisioning/backup/recovery and owner-visible migration
are explicitly resolved. See the earlier
[identity and trust design](experimental/pki/INTEGRATION-DESIGN.md).

## Target build and evidence boundary

The existing compile-only build script accepts `--pki-groundwork`; all other board,
SDK, module, source epoch and TX arguments remain the documented normal build
arguments. The default is off. The Kconfig hook selects SHA256, HMAC and CCM and
rejects PSA persistent storage or LTO for this qualification build. An inert
function-pointer table `mt_pki_linkage`, retained via linker `--undefined`, keeps
backend/session/ACK methods and the separate identity journal, candidate lifecycle
and real Monocypher identity deriver present in the ELF. It has no runtime callers,
startup initialization, instantiated provider, private key or nonce source. The
identity entry points remain dormant, including all storage access; retaining
their machine code does not provision or load an identity.

Both T096/nRF52840 and XIAO/ESP32-S3 must compile/link this option before claiming
target support. Per-function `.su` files for the new component report static
stack use; they are not whole-path worst-case stack or hardware measurements.
The class size is additional per session if a future application instantiates
one; no global session allocation is added by this build hook. RAM/flash and
retained-symbol reports belong with the exact source/binary build receipts.
These inactive images are unflashed and are not runtime private-message releases.

Host verification uses actual pinned PSA plus the unchanged pinned stock oracle,
failure injection, lifetime-state tests and sanitizers. Reproduction instructions
and evidence limits are in [tests/pki/README.md](tests/pki/README.md). Actual MCU
crypto timings, interrupt/stack behavior, stock radio interoperability, persistent
key entropy and recovery, and private messaging from stock apps are unqualified.
