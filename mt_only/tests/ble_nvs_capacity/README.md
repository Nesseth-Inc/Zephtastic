# M104 shared native Settings capacity model

Compile the actual pinned NVS engine and settings save/init against synthetic NOR with run.py --zephyr <isolated patched SDK> --output <fresh output>. No hardware or real keys are accessed.

T096 has four4KiB sectors in its16KiB DT storage partition. XIAO has eight4KiB sectors. CONFIG_SETTINGS_NVS_SECTOR_COUNT=8 is a requested ceiling, not evidence that T096 has32KiB available. Run both actual backing sizes.

37 synthetic names include all13 MT journal/legacy records at current maximum sizes, native id/irk/name/hash, and five peers with56-byte opaque keys,8-byte CCC values,4-byte Service Changed and2-byte Client Features values. Target qualification must check the actual native key ABI (68-byte object minus12-byte storage offset), two static CCC descriptors times4bytes, feature sizes and the app's18-byte derived device name. The IRK reservation is conservative; local privacy is disabled. These bytes are synthetic and never loaded as valid cryptographic bonds.37names stay below the raw boot64-name bound. A larger GATT table, local name or native key ABI requires a new capacity check.

1200mixed writes check every baseline record after each write; remounts and every before/partial/after write/erase boundary of a collecting ledger update exercise unrelated record preservation. Separate full-store cases expect ENOSPC and verify all baseline values across remount. The test requires actual collection; it does not demand an arbitrary erase count. These are software capacity/interruption observations, not physical wear/timing/brownout or permission to retry a latched application journal.
