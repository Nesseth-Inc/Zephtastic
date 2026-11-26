# M154 NVS contact-key scan cases

This host case exercises `NvsBootSettings` against the actual pinned Zephyr NVS/settings implementation and the existing synthetic NOR bridge. It creates canonical 640-byte contact snapshots, preserves the original six identity/ledger/nonce key positions, and checks holes plus an unknown setting without changing NVS state.

The case also rejects 639- and 641-byte values for either contact key, verifies unknown names cannot be read through the allowlisted boot adapter, confirms absent contact records return `-ENOENT`, and checks partial legacy-ledger name/value tombstones remain fail-closed. It does not mount, write, repair, or migrate contact storage through `NvsBootSettings`.

Root can run it with the existing runner and a new output directory:

```sh
python3 mt_only/tests/pki_storage_nvs/run.py \
  --zephyr /absolute/path/to/the-exact-pinned-zephyr-source \
  --output /absolute/path/to/a-new-m154-contact-scan-run \
  --cases "$PWD/mt_only/tests/m154_nvs_contact_scan_cases.cpp"
```

The case has not been compiled or run by this agent; the Root runner owns actual validation.
