# Warm peer metadata

The opt-in private history profile retains 100 evicted peers on T096 and 2,000 on XIAO. Each 40-byte record matches the layout of Meshtastic’s pinned WarmNodeEntry at commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. Timestamps round down to a 128-second boundary. The remaining bits describe the advertised role, prior protection category and prior verification. Each file holds 50 records. One worker call reads at most one 2,024-byte page and may atomically replace it. Files live in the private root, outside the phone file list. The phone continues to enumerate its existing thin and hot directory.

Thin-directory eviction commits a pending demotion in its own manifest. On reboot, the worker can repeat it until the warm page is durable. Warm entries first replace empty slots, then the oldest keyless entry, then the oldest keyed entry if the incoming node carries a key. Ties retain upstream physical-slot order. Three independent identifiers bind warm data to the local owner, channel domain and private directory lifetime. A four-ID withdrawal log retires individual removed, ignored or owner-edited IDs. A reset or unobserved withdrawal backlog exceeding four retires the entire obsolete generation. A warm storage failure retries after a minute. The thin directory continues updating during that interval and retains its last pending demotion. Further evictions during this fault interval can go unrecorded by the warm tier.

For incoming public Position broadcasts, TrafficManagement can request an asynchronous cold role lookup. A later duplicate can use that role’s stock interval cap. A fresh hot NodeInfo takes precedence. Freshly admitted hot peers can also recover a missing rounded last-heard time after their NodeJournal enrollment commits and their key matches. The warm lookup cannot supply a DM encryption key, admit an unknown peer, or mark a signature as verified. The pinned upstream firmware can immediately consult its in-memory warm table for these operations. ZT’s first-packet routing, cryptographic decisions, and current signer evidence keep their existing providers. If a reboot interrupts consumption after durable removal and before application, that historical timestamp can be lost.

Run against the pinned upstream Meshtastic checkout and Zephyr's LittleFS source tree.

```sh
python3 mt_only/tests/stock_warm_peers/run.py \
  --stock "$MESHTASTIC_FIRMWARE" --littlefs "$LITTLEFS_SOURCE"
```

The runner compiles upstream placement, packed metadata and consumption from the specified Git object, compares 2,500 mixed operations, and fills both target capacities. Native LittleFS uses 4 KiB flash sectors, 16-byte programming and 64-byte caches. The suite tests targeted revocation with an unrelated peer present at every program/erase boundary, including torn writes, and compares intact V2 migration, reboot replay, changed domains and re-key rejection. The radio fixture drives encoded packets through Service::rx. Both executables run with AddressSanitizer and UndefinedBehaviorSanitizer using normal and short-enum ABIs. These fixtures use synthetic flash and radio input.
