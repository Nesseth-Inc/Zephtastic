# Preserved-root bounded boot

`PkiStorage::prepare()` keeps the existing legacy boot path when both nonce
slots are positively absent. When a nonce slot is present, it requires a
current valid identity and an append store. It reconstructs the fixed legacy
snapshot from the immutable append root, validates the root against the
identity, replays the complete event area, and requires the committed BMIT
selector with no remaining events. Only then does it bind and open the
existing two-slot nonce counter. The integrated contact-layout check then
validates both original NVS ledger values as exact legacy, canonical pending
genesis, or a committed adjacent contact pair. `Prepared` is published last.

Root reconstruction does not trust the redundant NVS values as the original
baseline. The following layout check reads them and rejects missing/corrupt
membership state. Neither step rewrites NVS, mutates the append store, or
initializes, repairs, or replaces nonce
records. Missing or malformed counter records, root/replay/identity errors,
an old selector, or a missing append store fail closed. The existing bounded
nonce allocator remains the authority for transmit IDs; legacy history stays
available for replay compatibility and effective peer membership.

The counter presence check runs through the existing boot settings reader under
the boot lease. The Zephyr scan admits exact 64-byte marker and 640-byte
contact lengths in the original ledger value slots; semantic admission remains
in the contact-layout validator. See `docs/M154-CONTACT-STORAGE.md` for the
integrated in-place migration and runtime membership contract.
