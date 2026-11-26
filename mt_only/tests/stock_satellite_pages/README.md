# Public satellite report storage

Run the host suite from the repository root with the two pinned donor checkouts.

```sh
python3 mt_only/tests/stock_satellite_pages/run.py --stock /path/to/meshtastic --littlefs /path/to/littlefs
```

The runner extracts NodeDB's eviction code from Meshtastic `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`, verifies the four source hashes from LittleFS `e9a8638fc228e43a3156ed3ac602229af3e3c1d5`, and runs ASan and UBSan with both host enum ABIs. The firmware's private recovery wrapper checks the Zephyr adapter source hash at configure time.

| Paged private volume | Admitted remote peers | Warm records | Position, device, environment, status | Filled volume with two small public files |
| --- | ---: | ---: | ---: | ---: |
| T096, 18 × 4 KiB, 128-byte cache | 119 | 100 | 40 each, six mixed pages | 15 blocks |
| XIAO, 128 × 4 KiB, 64-byte cache | 199 | 2,000 | 199 each, 30 mixed pages | 91 blocks |

The donor's high-memory map limit is 250. The 8 MiB XIAO's hot directory holds 200 nodes including self. Upstream removes satellite state on eviction and prunes orphan reports, giving this board 199 eligible remote owners. T096 uses the donor's 40-entry limit independently for each kind. Donor eviction uses the current owner's last-heard time and numeric node order for ties. ZT preserves its established local radio admission, PKI ownership and phone access policy.

The archive stores only canonical public-broadcast observations of an admitted peer. Page identity includes the local identity and channel domain through the directory incarnation. Each row records the full peer public key, enrollment lifetime, slot, channel and public receive metadata. Key rotation, removal and same-key reenrollment invalidate old or queued rows. Prior directory formats and snapshots remain intact. A recognized foreign directory cannot acknowledge a new journal fence, including a previously primed generation. The original firmware profile can read its own files.

When a settings edit or unavailable volume defers an admitted hot report, the owner reconciles the current hot samples and private tombstones after a verified bind. A channel change clears the old live observations. Directory-open and stage-cleanup failures leave the archive eligible for its next timed retry.

The suite checks all 83 IEEE floating fields against the previous numeric policy, including presence, signed infinities, quiet and signaling NaNs, signed subnormals and finite extremes. The two enum builds each run 1,980 comparisons through the canonical telemetry wire codec.

The public wire path exercises actual RF frame decode, absent-hot-peer admission, PositionLite time retention, channel changes, directed status replacement, cold PhoneAPI replay and transport backpressure. Report storage reads occur in the worker. Accepted pending updates coalesce by peer and kind. The allocation test saturates four keyed pending report slots and both backpressured phone readers. The owner requests at most 1,792 bytes from the system heap, excluding allocator bookkeeping. On heap exhaustion it returns backpressure and releases all allocations on close. The four-entry receive queue falls back to hot-cache retry under pressure. An incoming cold-only update without a free queue slot is best effort. A backpressured phone keeps its pending frame and validates the current peer before emission. Disk read faults end that pass and a later pass can retry.

The NOR test fills all thin, warm and report slots, including 233-byte environment records. It exercises actual file writes, sync and rename plus every program/erase failure boundary during report replacement. It accepts only the prior or new complete page after power loss. Transient program errors abandon dirty open handles and remount from durable media before normal writes resume. Each report page has a BLAKE2b-128 digest of its complete header and body. Staged private data cannot grant trust or repair a damaged manifest. The file tests do not drive a radio or interact with physical flash.

At full T096 occupancy, two 4-byte public entries and one open, synced 4,096-byte upload coexist with serial private report, warm and thin updates. In this image a 4,097-byte upload needs a second data block. The upload can still fit while a concurrent thin update returns ENOSPC. Canceling the upload frees the space. A verified directory reopen then reclaims only abandoned stages and unselected page banks. The original public destination, committed peer rows and previous withdrawals survive, and normal updates resume. Committing a large public file can leave space pressure until the owner releases storage. The XIAO volume passes the same tests with an 8,000-byte upload. These figures describe the tested population, filenames, geometry and cache. Larger public trees also consume metadata blocks.

The tests retain the 19-sector T096 candidate and a 64-byte-cache 18-sector control for comparison. The 128-byte cache allows the warm index to remain inline. At 18 sectors its report replacement has 72 abrupt/torn cases and 70 same-boot transient error cases. The XIAO sweep has 136 abrupt/torn and 134 transient cases. Flash and static RAM headroom come from the exact cross-target link. The full T096 build uses a 16,384-byte main stack. These host tests do not measure its runtime high-water mark.
