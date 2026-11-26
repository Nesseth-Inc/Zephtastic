# Protected NodeDB on the XIAO

The full paged XIAO keeps up to 199 remote working entries and one self entry. Favorites, ignored peers and manually verified keys share the stock limit of 198 distinct protected peers. A second flag on a protected peer consumes no additional slot. A NodeDB entry records an observed or locally supplied public key. Private admission still requires the separate enrolled-contact registry, limited to 16 contacts and four active sessions.

The first local administration request needing more than 14 protected peers creates the extended records. Before that request both journal selectors use the existing inline format. The additional files are two 19,351-byte records and one temporary replacement on the existing 512 KiB private LittleFS volume. Both original NVS selector names are retained. A selector binds each data file to the device node, slot and a random 128-bit incarnation. Writes sync and atomically replace the inactive record, then verify the complete record and generation before changing live policy. Conflicting file contents, unexpected storage errors or a missing selected file hold NodeDB-dependent RF admission. A pre-commit space refusal leaves the prior verified policy active.

For an application-only downgrade, use normal owner operations to remove temporarily added peers. When at most 14 are protected and at most 16 total remote entries remain, the editing transaction commits and verifies both legacy slots. If extra ordinary peers remain, remove them explicitly or use the existing NodeDB reset with the intended keep-favorites policy. Removing an enrolled private contact has its ordinary trust-retirement effect. Radio discovery can be paused during the edit to avoid admitting new ordinary peers. The pre-migration binary deliberately refuses an active extended selector. The conversion writes only the public NodeDB namespace. It does not restore identity, private-contact slots, append history or cryptographic nonce counters.

Run the pinned source and storage checks from the repository root.

```sh
python3 mt_only/tests/stock_protected_nodes/run.py --stock "$MESHTASTIC"
python3 mt_only/tests/stock_peer_directory/run.py --stock "$MESHTASTIC"
python3 mt_only/tests/stock_satellite_pages/run.py --stock "$MESHTASTIC" --littlefs "$LITTLEFS"
```

The first command compiles the pinned stock flag admission functions and runs both native enum ABIs. The previous ZT reader opens the original records, refuses a live extended selector, and opens the downgraded records. The second command verifies both cold phone handshake modes with 198 protected entries, one ordinary entry, self, and transport backpressure. The final command uses the production filesystem adapter on synthetic 4 KiB NOR with the complete XIAO thin, warm and report stores, two public files and an upload up to 8,000 bytes. It interrupts every flash mutation of a complete replacement and checks each committed record on remount.
