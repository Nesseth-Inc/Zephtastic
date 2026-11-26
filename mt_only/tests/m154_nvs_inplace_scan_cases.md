# M154 in-place NVS scan-size cases

This fixture reuses the actual pinned Zephyr NVS/settings runner and the existing synthetic NOR bridge. It checks that the original `mtpki/ledger0` and `mtpki/ledger1` names retain their 1944-byte caller capacity while allowing exact 64-byte migration markers and 640-byte contact snapshots to be read at their actual lengths. It also rejects nearby lengths 63, 65, 639, and 641 without modifying flash.

The adapter performs size admission only. These tests intentionally use synthetic value bytes; `PkiStorage`/the in-place state classifier remains responsible for validating marker, ledger, and snapshot contents. Existing tests continue to cover ordinary 1944-byte and historical 1752-byte ledger rows, the two new contact keys, and the fixed 64-name scan.

Root can run the fixture with the existing runner and a new output directory:

```sh
python3 mt_only/tests/pki_storage_nvs/run.py \
  --zephyr /absolute/path/to/the-exact-pinned-zephyr-source \
  --output /absolute/path/to/a-new-m154-inplace-scan-run \
  --cases "$PWD/mt_only/tests/m154_nvs_inplace_scan_cases.cpp"
```

No compile or test was run while preparing this case; Root owns actual validation.
