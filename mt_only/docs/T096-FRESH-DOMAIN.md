# Full companion on a freshly provisioned T096

## Extended companion with a separate first-install layout

The build switch `--t096-full-companion` selects all onboard companion modules, the development modules and games, rollback-aware device reset, LittleFS peer history and the local phone file interface. It uses the following nRF52840 internal flash partitions. The 680 KiB profile stays available through `--t096-fresh-domain`.

| Region | Start | End, exclusive | Size |
| --- | ---: | ---: | ---: |
| Application | `0x26000` | `0xdc000` | 728 KiB |
| Settings and fresh PKI | `0xdc000` | `0xe0000` | 16 KiB |
| LittleFS history and public subdirectory | `0xe0000` | `0xf3000` | 76 KiB |
| Device reset floor | `0xf3000` | `0xf4000` | 4 KiB |
| UF2 bootloader reservation | `0xf4000` | `0x100000` | 48 KiB |

The 728 KiB application overlaps the previous settings partition and part of the old LittleFS volume. First installation requires a complete restorable backup and an independently identified device. Run `python3 mt_only/scripts/check-t096-fresh-layout.py --full-companion READBACK.bin` on the **original** 1 MiB readback. This checks the original PKI, settings, filesystem and retirement page before any image can overwrite them. A nonblank region requires preservation and a separate migration decision. Switching between these layouts on an enrolled device likewise requires a recovery plan.

For offline builds use the normal T096 toolchain paths with `--t096-full-companion`. The pinned LittleFS source belongs at `fs/littlefs` in the module tree. The optional `--initialize-erased-satellite-history` allows the firmware to initialize a volume only after the device reset floor is active and the entire selected volume reads erased. Mounting an existing compatible volume uses the filesystem's no-format policy. Private history and the phone-visible `/static` tree use separate directories. The USB transmit queue on this profile is 2,048 bytes. The transport checks room for a complete frame before queuing it and resumes the configuration stream as the host drains it.

The full profile packs 32 thin peer records into a flash sector. The original profile uses 16 records per page. A recognized original directory found under the new geometry returns a layout mismatch and remains available for recovery with the matching profile. The 728 KiB geometry requires a fresh installation even when an older filesystem appears mountable.

## Paged report history on a fresh T096

Use `--t096-paged-companion` for an application partition of 732 KiB. The application starts at `0x26000` and ends at `0xdd000`. Settings occupy `0xdd000` through `0xe0fff`. LittleFS uses the eighteen 4 KiB sectors from `0xe1000` through `0xf2fff`. The retirement record and UF2 reservation stay at `0xf3000` and `0xf4000`. The 680 KiB and 728 KiB builds remain independently selectable. Each layout has its own storage geometry. Use the same complete original backup and offline `--full-companion` check described above before a future first installation.

The paged profile retains up to 40 public reports of each kind (Position, DeviceMetrics, EnvironmentMetrics and Status), 119 remote directory entries and 100 warm peers. The extended XIAO with `--paged-satellites` retains 199 eligible reports per kind and 2,000 warm entries. Retirement, channel changes and peer key changes invalidate old reports. For an exact capacity comparison, the host tests populated all three stores and two public files on T096. They could replace a private page while a 4,096-byte public upload was open. At 4,097 bytes, a thin-directory write ran out of space and later succeeded after the upload was released. This is a shared volume. The available upload size depends on its contents. [Power-loss, recovery and capacity tests](../tests/stock_satellite_pages/README.md)

## Original 680 KiB first-install profile

The build option `--t096-fresh-domain` uses the native NVS-based identity, nonce and contact format. It gives the application 680 KiB from 0x26000 to 0xd0000. The 16 KiB settings partition starts at 0xd0000 and LittleFS starts at 0xd4000. Those two locations match the legacy layout.

## State and capacity

The legacy layout dedicates 168 KiB to an immutable 4 KiB root and 2,624 append records of 64 bytes. Its identity and nonce ledger are bound to this history. Once a legacy identity switches to the bounded nonce counter, its pre-switch history still fences old packet IDs. The existing legacy image and recovery tools retain their original partition addresses.

A new identity needs **zero bytes of dedicated append history**. Its identity-bound selector, intent, nonce high-water and contact directory share the existing four 4 KiB NVS sectors. The counter alternates two 64-byte records and reserves 256 future values before issuing a value from a new block. A reboot skips the unused part of the last block. The directory retains up to 16 peers. The received-message duplicate window is volatile across reboot. Revoked contacts and the outgoing nonce high-water survive reboot. NVS garbage collection reclaims old records.

With all 16 contact slots populated, the mounted fixture measured **1,632 bytes of live PKI values**.

| Record | Copies | Bytes per copy | Total bytes |
| --- | ---: | ---: | ---: |
| Contact snapshot | 2 | 640 | 1,280 |
| Nonce high-water | 2 | 64 | 128 |
| Identity | 1 | 84 | 84 |
| Provisioning intent | 1 | 84 | 84 |
| Fresh-domain selector | 1 | 56 | 56 |

The seven key names use another 104 bytes and the name counter uses two. Including four-byte write alignment, the live records occupy 1,752 data bytes plus fifteen 8-byte NVS allocation entries. Zephyr reserves one sector for garbage collection and keeps sector bookkeeping in the rest. Its free-space function measured 12,232 bytes on an empty mount and 10,344 bytes with these records present.

The fixture also added 33 synthetic settings with the base, node journal, module settings and five BLE bond record sizes from the existing capacity test. After remount, it recovered all 33 and reported 4,184 bytes free, with 6,671 live value bytes across the PKI and other settings. On the history profile, two 1,572-byte v7 NodeJournals and its 16-byte epoch leave **3,980 bytes free** with the same PKI contacts and BLE bonds. The peer directory and observation records use the separate LittleFS volume. This measures the specified population. Other enabled features and settings can consume additional space.

The fixture verified USB generation and confirmation, refusal of a seventeenth contact, durable revocation, and two independent recovery refusals. One fixture contains a malformed selector. A second contains only a valid old identity with no selector or intent. Boot and fresh staging refused both without writing. With PKI alone, 384 reboots and 1,536 TX reservations caused eight modeled sector erases. Field endurance depends on reboot frequency, nonce reservations, other settings traffic and physical flash wear.

## First enrollment

Use this profile for a unit whose existing history and settings may be proven erased. The image occupies the old history addresses. Its runtime cannot protect content that a bootloader already overwrote. For a first installation, identify the T096 independently and retain its complete restorable backup before changing the board. Check an offline 1 MiB raw readback with `python3 mt_only/scripts/check-t096-fresh-layout.py READBACK.bin`. The checker reads the supplied file, requires an occupied bootloader reservation, and tests every byte of the old append region and settings. A result applies to that capture. The checker can neither identify the physical board nor authorize an install.

The optional `--t096-device-reset` build preserves this application extent. Its first installation additionally requires that the entire original LittleFS region is erased. Add `--device-reset` to the offline checker. This layout reserves the last 4 KiB below UF2 as an append-only retired-identity record. Factory reset behavior and limits are documented in [Upstream Admin operations](UPSTREAM-ADMIN-OPERATIONS.md).

If either region is occupied, keep using the legacy layout for that identity. This experiment provides no in-place conversion from a legacy packet history. A fresh-domain image refuses to import or reinterpret a legacy identity. On a blank unit the normal USB fresh-provisioning stage and confirmation create a new identity, then reboot opens the new domain. The first-enrollment check intentionally refuses a subsequently provisioned unit. An upgrade that keeps an existing fresh-domain identity requires separate verification of its installed layout and recovery state.

## Building the original and extended profiles

Use the repository's pinned Zephyr tree, module checkout, Nordic module checkout and Zephyr SDK. The extended onboard profile includes TAK, the GPIO handlers and passive BLE Paxcounter. GPIO operations still require an authorized pin and the observer still requires its runtime setting. The initial-release profile uses the previous companion feature selection and leaves additional memory for runtime use. Pass `--initial-release` to the same command with a separate output directory to build that version.

```sh
python3 mt_only/scripts/build-local.py t096 \
  --zephyr "$ZEPHYR_TREE" --modules "$MODULES_TREE" \
  --nordic-modules "$NORDIC_MODULES_TREE" --sdk "$ZEPHYR_SDK" \
  --build "$BUILD_DIR" --normal-tx --stock-pki-trial \
  --lto-runtime --t096-fresh-domain
```

Both profiles build a UF2 for the larger fresh-domain application partition. [Measurements and port coverage](T096-SIZE-AND-UPSTREAM-PORTS.md)
