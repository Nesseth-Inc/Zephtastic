# M154 in-place contact lifecycle fixture

`m154_inplace_storage_lifecycle_tests.cpp` includes the existing synthetic
preserved-root fixture with its entry point renamed, so the expensive full
append-history setup is created once. It then exercises migration, pending
marker refusal and resume, contact add/revoke/re-add, bounded replay, full
16-peer capacity, reboot persistence, and continuation of the existing global
nonce counter through `PkiStorage`.

The fixture is host-only. It verifies that identity rows, nonce rows, and the
immutable append bytes are unchanged by contact migration and membership
churn. It does not exercise Zephyr NVS physical wear, target memory fit, USB
dispatch, or hardware behavior.
