# HopScaling restart qualification

Run the following command on the private native qualification runner. It needs Python 3, host C/C++17 compilers with ASan and UBSan, the pinned Meshtastic firmware at `d5f16191ca28b01b5024f590f04a67f25d66f1b1`, and the Zephyr checkout prepared from `mt_only/dependency-lock.json` with its overlays. All data is synthetic. The runner writes its report to the requested path.

```sh
python3 mt_only/tests/stock_hop_restart/run.py \
    --stock "$STOCK_ROOT" --zephyr "$ZEPHYR_BASE" --output "$OUT/hop-restart.json"
```

Without `--zephyr` or `ZEPHYR_BASE`, the donor comparison still runs and the NVS result is `NOT_RUN`. Full qualification requires `nvs.status == "PASS"`, both sector counts and an exact target link.

The filesystem fixture captures and supplies the donor's actual file bytes. Both enum ABIs run its loader, scheduler, saver and hop recommendation across cold and warm boots, including a restart with no new RF samples. The test checks all 522 donor bytes at each roll. ZT appends a two-byte CRC in the single native key `mthop/state`. Its scratch image is 524 bytes on the caller's stack. Boot keeps sampling if the key is missing, corrupt or unreadable. A malformed or unread key disables checkpoint writes for this boot. Factory and device reset own the key even in firmware compiled without warm history.

The NVS fixture runs the actual Zephyr settings/backend/flash-management code using the shared byte-addressed NOR model. It fills four 4 KiB sectors for T096 and eight for XIAO with modeled PKI, both NodeJournal slots, waypoint history and five BLE bonds. It replaces the checkpoint under load, reopens after each before/partial/after injected failure during creation and garbage collection, compares all unrelated values, and exercises a large node update, reset cleanup and full-store refusal. Its erase counts describe the modeled population. Physical endurance, radio downtime and stack high-water marks require measurements on hardware.
