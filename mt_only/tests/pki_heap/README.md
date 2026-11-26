# Crypto allocation regression

Run with Python3, CMake and Clang using the exact pinned TF-PSA checkout:

```
python3 run.py --source /path/to/source --tf-psa /path/to/pinned/tf-psa-crypto --output /tmp/pki-heap-review
```

This host configuration enables CMAC, generic cipher CCM, static PSA slots and exclusive buffers like the reviewed target algorithm path. Four scratch C translation units receive scoped allocation hooks; the global allocator and firmware are unchanged. The synthetic CMAC operation remains open while private CCM executes, measuring overlapping contexts. Tests inject 513 CCM allocation failures and a CMAC second-allocation failure and require cleanup, unchanged CCM caller output, and later recovery.

Host sizeof values differ from the MCU ABI. Target sizes are measured separately (AES280B, CMAC36B in the reviewed build). This is not a threading, physical heap fragmentation, stack high-water or latency qualification. External RNG always refuses; fixtures use deterministic public test constants only.
