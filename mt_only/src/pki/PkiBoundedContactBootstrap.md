# Bounded contact bootstrap composition

`PkiBoundedContactBootstrap::prepare()` is a private cold-boot composition seam; it is not wired into application startup. The caller must serialize it against settings/storage mutation and RX startup and must supply an already loaded, validated `IdentityJournal`.

The helper performs this sequence once:

1. Read the immutable append root and reconstruct its canonical ledger snapshot using the current identity node and public key. The root contains no nonce-record binding.
2. Load the validated snapshot into the helper's private RAM-only ledger. Its `anchored_` guard rejects ordinary ledger writes.
3. Open and fully replay the append journal. Readiness requires the BMIT counter marker, zero remaining cells, the expected format, and the final generation `root generation + 2624`.
4. Recheck the same active identity, then open both nonce-counter slots against the exact existing binding tuple: local node/public key, replayed generation, and immutable root CRC. No counter initialization, repair, or write occurs during this step.
5. Publish `Ready` only after the nonce store reports an active matching allocator. All status, peer, and nonce-reservation observation remains unavailable before then. Any refusal closes the staged history/ledger/counter view and is terminal for that helper instance.

The current counter record does not bind the IdentityJournal generation or a separate per-peer domain. This helper does not invent either binding. Callers must retain the independent validated current IdentityJournal and the current counter-record format. CRC validation detects accidental corruption; it does not establish an anti-rollback root of trust.

This composition is not a storage migration or runtime integration. It does not load settings, modify NVS or append storage, reset/recreate identity, publish a runtime contact overlay, or prove target stack/fit.
