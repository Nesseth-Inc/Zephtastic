# XIAO ESP32-S3 companion updates

The signed release includes the ordinary companion, its radio and phone interfaces, Wi-Fi, and the Meshtastic update transports. Every device in a release series trusts the same P-256 public key. The signer stays with the release owner. Use the full XIAO ESP32-S3 board profile and the repository's pinned Zephyr SDK, modules and MCUboot tree.

The XIAO bootloader uses 64 KiB flash block erases when swapping signed application images. This shortens the reboot while the stock Android updater waits for the companion to reconnect. The change ships in the bootloader portion of a local installation. Wireless application updates leave an existing bootloader in place. For an enrolled unit, use the verified recovery and data-preserving local installation procedure below before changing its bootloader.

## Build a release

```sh
python3 mt_only/scripts/xiao_release.py build \
    --zephyr "$ZEPHYR_BASE" --modules "$ZT_MODULES" --mcuboot "$MCUBOOT" \
    --sdk "$ZEPHYR_SDK_INSTALL_DIR" --source-date-epoch "$SOURCE_DATE_EPOCH" \
    --public-key release-public.pem --build build-xiao
```

Supply `build-xiao/app/zephyr/zephyr.bin` to the release signer. The signed version must advance the major, minor, revision or build number. MCUboot imgtool uses a 32-byte header already reserved in that binary, 4-byte alignment, maximum alignment 8, a 0x1c0000-byte slot, 448 sectors and an ECDSA P-256 key-hash TLV. The following command runs in the release owner's signing environment.

```sh
imgtool sign --key "$RELEASE_SIGNING_KEY" --version "$VERSION" \
    --header-size 32 --align 4 --max-align 8 --slot-size 0x1c0000 --max-sectors 448 \
    build-xiao/app/zephyr/zephyr.bin xiao.signed.bin
python3 mt_only/scripts/xiao_release.py pack --build build-xiao \
    --public-key release-public.pem --signed xiao.signed.bin --out xiao-release
```

The packager checks the linked layout, full application profile, public key in both binaries, signature and exact application body. It derives each artifact from the same signed application and records its purpose, size and SHA-256 in `manifest.json`.

Use the exact firmware target reported by the node. It is `mt-zephyr-xiao` for the ordinary release and `mt-zephyr-xiao-198` for the expanded release. The installed bootloader must trust the release signer.

| Filename | Destination | Use |
| --- | --- | --- |
| `firmware-<target>-<version>-update.bin` | Inactive slot through Wi-Fi or BLE | Select this local file in the stock Android updater. Its manifest purpose is `wireless`. |
| `firmware-<target>-<version>.bin` | Inactive slot through Wi-Fi or BLE | In Files on iPhone, save this file in the Meshtastic app's Documents folder, then open its firmware update list for the connected node. Its manifest purpose is `wireless_apple`. Both wireless files contain identical bytes and have the same SHA-256. |
| `<target>-<version>-usb-update.bin` | Inactive slot at `0x10000` | Use the stock Meshtastic ESP USB update script with the S3's explicit serial port. The padded image includes a test-boot trailer. |
| `<target>-<version>.factory.bin` | Entire 8 MiB flash | First installation on blank or separately backed-up hardware. This includes the bootloader and a confirmed primary image. |

Keep factory images from older packages outside the Meshtastic app’s Documents folder. Their `firmware-` prefix can make the stock iPhone scanner offer them as updates. The recovery verifier accepts those packages under their original filenames.

## Update an installed node

Identify the S3 and its boot layout first. An existing address-zero Zephyr simple-boot installation requires a verified backup of its installed firmware, identity, and settings before migration. Writing a stock serial update at `0x10000` on that original layout overwrites the running application. A full factory installation erases storage. Retain the original recovery image and use a backup-and-restore migration when preserving an enrolled identity.

For a wireless update, submit the stock `AdminMessage.ota_request` through an authorized local connection with `OTA_WIFI` or `OTA_BLE` and the 32 raw SHA-256 bytes of the wireless file. Wi-Fi uses the node's configured station and TCP port 3232. Bluetooth uses the stock Unified OTA service and the device's configured pairing policy. The maintenance window closes after completion or timeout. The device verifies the bytes, signer and image version before requesting a trial boot.

For a wired update from a confirmed, idle signed installation, run the upstream `device-update.sh -p "$S3_SERIAL_PORT" -f "$USB_UPDATE_FILE"`. The stock ESP script writes at the beginning of the secondary slot. A pending trial must finish before another update. After either method, reconnect to the companion, read its firmware version and check the node identity and configuration. Successful startup confirms the trial image. MCUboot can revert an unconfirmed trial on a later boot.

### Stock mobile updater qualification on the XIAO

The firmware baseline is Meshtastic `v2.8.1.8e6a88d`. The stock Android sender and Apple's Swift sender both completed signed BLE updates on the physical S3 using a Linux Bluetooth central. Both clients discovered the station updater over Wi-Fi, and Apple's sender completed a signed transfer to TCP 3232. The Apple tests exercised the transport and file-selection code separately from the iPhone UI.

The BLE control connection and the update connection have separate settings. On the test central the updater changed the link from 45 ms and 420 ms supervision to the stock ESP32 request of 15 ms and four seconds, both with zero latency. A full 1,599,260-byte signed image returned OK in 120 seconds with the stock parameters. A separate sustained transfer used 79,954 20-byte application writes on an MTU-517 connection and returned OK after two hours and one minute. A native MTU-23 control exercised fragmented START and four acknowledged 20-byte writes.

On firmware `8e2a66f3`, Android received `VerificationFailed` for a complete 1,599,482-byte image signed by a different key. An independent flash read confirmed the original primary and the foreign image in its unselected secondary slot. On the same firmware, the properly signed 1,599,259-byte control returned OK in 127 seconds. The client had previously timed out on the foreign file. The updated receiver leaves the error notification available for the same two-second window as success and waits for the disconnect callback before rebooting, subject to a five-second bound. A separate mid-transfer supervision loss was observed even with the four-second setting. After an interrupted session, verify the running image and pending-slot state before freshly authorizing a retry from byte zero. [Carrier tests and stock client behavior](../tests/stock_signed_ble_ota/README.md)

## Opt in to 198 trusted private contacts

On a blank factory installation, the expanded firmware prepares its 512 KiB LittleFS volume at `0x400000` during startup, before accepting contact edits. This volume stores the large node directory, including favorite and manual-verification flags. Firmware mounts an existing filesystem in place. If mounting fails, it formats only after reading the entire partition successfully and finding the erase value in every byte. An unreadable or unrecognized nonempty volume needs recovery before the large directory is available. The separate trust banks start at `0x480000` and still require explicit USB activation.

The ordinary signed XIAO stores 16 private contacts. The 198-contact release has its own signer, a 32 KiB partition at `0x480000`, and the PhoneAPI target `mt-zephyr-xiao-198`. Both profiles use four active private sessions. Changing capacity preserves the requirement for a trusted key for each private destination.

Pass `--xiao-expanded-contacts --ordinary-public-key ordinary-release-public.pem` to both the build and pack commands above. Use the expanded release public key with `--public-key` and a separate build directory. Packaging verifies both signer identities and the linked flash map. Both wireless filenames contain the complete `mt-zephyr-xiao-198` target.

1. Before conversion, record the board's USB identity, node number, public-key fingerprint and settings while running its confirmed ordinary image. Disable TX, quiesce all writers and make two independent reads of the entire 8 MiB flash in the same ROM session. Keep an encrypted recovery copy and the separately recorded hashes. Keep the ordinary release package corresponding to the confirmed primary.
2. Decrypt the copy into the verifier through a pipe. Supply the two original ROM hashes and the digest of the trusted ordinary release manifest. The verifier checks all 8 MiB, the confirmed primary, the ordinary bootloader, the inactive trial marker, the unused contact region and the presence of settings. An example invocation follows this procedure.
3. Changing signer requires a local USB installation. On an enrolled signed-layout device, preserve the verified settings, identity retirement floor, filesystems and all other data when replacing the bootloader and application slots. A factory file written at address zero replaces the full flash. Verify the new bootloader, confirmed application and preserved data through an independent device read, then verify the node identity over USB.
4. The new image starts with the original 16-key authority. Read `zt_pki_contacts_expand_v1 status` on the local USB owner session. For explicit conversion send `zt_pki_contacts_expand_v1 <eight-digit-lowercase-node> <64-digit-local-public-key>`. It verifies the existing pins, persists the new selector and requests a reboot. Reconnect, check that capacity is 198 and the trusted count matches the original. Stock local `Admin.add_contact` then applies to the expanded store.
5. To return to older firmware, first verify recovery again. Restore a pre-conversion image with the same identity only when its private nonce and replay state are current and all its trusted keys are still authorized. After private traffic, key rotation, revocation or uncertain history, use verified recovery with a fresh identity and enroll only currently authorized keys. An old settings snapshot can otherwise reuse a nonce or reinstate a removed key. Preserve the previous installation's encrypted recovery copy until the replacement boots, its identity and settings have been read back, and the correct peers are enrolled.

```sh
your_authenticated_decrypt_command | python3 mt_only/scripts/xiao_recovery_verify.py \
    --ordinary-manifest ordinary-release/manifest.json --manifest-sha256 "$TRUSTED_MANIFEST_SHA256" \
    --first-rom-sha256 "$FIRST_READ_SHA256" --second-rom-sha256 "$SECOND_READ_SHA256"
```

The expanded bootloader rejects ordinary-signer updates. After conversion, older private-storage readers refuse the expanded selector. A raw USB programmer can replace those protections, which is why returning to older firmware requires the recovery procedure.

### Checking same-identity recovery before a write

The archive check above validates the saved release and two matching ROM reads at capture time. A device can reserve another block of 256 private nonces while its contact capacity and visible peer count stay unchanged. Restoring the earlier reservation can reuse counters for the same identity. Receive replay state and contact revocation have independent histories.

`pre-restore` is a read-only comparison. While the current expanded installation is quiescent, collect two independent, complete 8 MiB ROM reads of the physically identified XIAO and protect them using the same authenticated backup procedure. Keep the target in ROM or physically disconnected through any decision. The ordinary archive and both manifests require independently retained hashes. Feed authenticated decryptions directly to the comparison on stdin and a second inherited descriptor. The comparison needs neither a serial port nor a raw image on disk.

```sh
your_authenticated_ordinary_decryption | python3 mt_only/scripts/xiao_recovery_verify.py pre-restore \
    --ordinary-manifest ordinary-release/manifest.json --ordinary-manifest-sha256 "$ORDINARY_MANIFEST" \
    --ordinary-first-rom-sha256 "$SAVED_FIRST" --ordinary-second-rom-sha256 "$SAVED_SECOND" \
    --expanded-manifest expanded-release/manifest.json --expanded-manifest-sha256 "$EXPANDED_MANIFEST" \
    --expanded-first-rom-sha256 "$CURRENT_FIRST" --expanded-second-rom-sha256 "$CURRENT_SECOND" \
    --saved-state saved-public-state.json --current-state current-public-state.json \
    --current-fd 3 3< <(your_authenticated_current_decryption)
```

Each public state record needs the same independently established physical-target digest (`physical_target_sha256`), its own `full_read_sha256`, the live owner’s numeric `node` and `profile_owner_sha256`, `nonce_reserved_through`, and every authorized association as `authorized_pins` entries containing `node` and `public_sha256`, with `pinned_count` equal to the complete table size. Supply only public fingerprints. Set `authoritative_pin_membership` and `quiescent_acquisition` to true only if independently established for this capture. A general phone node list includes descriptive keys and cannot establish private authorization. If a trusted full membership inventory or quiescent read is unavailable, same-owner recovery remains closed.

The verifier validates both release profiles, requires the expanded signer to name the ordinary authority, and corroborates the owner, nonce domain, and contact table using canonical CRC-protected records. It compares every persisted byte from `0x390000` to the physical end of flash, including settings, replay history, scratch, the retirement page and the expanded bank. Even a harmless settings change can make the comparison inconclusive. The raw record scan can see historical NVS entries, so agreement with a counter or a physical contact record by itself cannot establish authority. USB and ROM observations depend on trusted physical acquisition and custody; a JSON file alone supplies neither.

A refusal exits **2**. Byte-for-byte persistent-state equivalence with all corroborating evidence exits **3** and is strictly informational. Neither comparison returns a write authorization or a successful shell exit. An activated expanded bank, changed peer association, changed reservation, or unverified history keeps same-owner rollback closed. Use the separate fresh-identity recovery path.

`compare-receipts --saved-state SAVED.json --current-state CURRENT.json` can explain a refusal using the existing ordinary backup and expanded USB observations without decrypting either archive. It always exits 2. Matching reported floors still require the complete comparison.

The pre-conversion archive has an erased 198-contact bank. Activating the expanded store programs that bank, so an activated installation cannot satisfy the byte-equivalence check. Equivalence is possible before activation if all persisted bytes stayed identical. A difference somewhere in persisted state makes equivalence inconclusive; only independent evidence such as a changed bound nonce reservation or revoked peer establishes the corresponding hazard. This tool does not migrate an active 198-contact authority back to 16 under the same identity. The supported return after activation uses a fresh identity and a new enrollment of currently authorized peers.
