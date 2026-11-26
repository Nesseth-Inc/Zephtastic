# Upstream Admin operations on the experiment branch

The reference is Meshtastic firmware commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. Its main Admin receive switch has **40 case labels**, including a module-config response and two platform-specific labels. The ten-way request-to-response classifier also covers requests handled outside this switch. ZT uses that classifier in its encrypted Admin response correlation. Board settings, peer trust and storage continue to use their Zephyr owners.

## Receive switch inventory

Names in the table omit the common protobuf prefix. A handler for a config message accepts the subtypes that its board, build and transport implement.

| Pinned switch labels | ZT handler and effect |
| --- | --- |
| `get_owner_request`, `get_config_request`, `get_module_config_request`, `get_channel_request`, `set_owner`, `set_config`, `set_module_config`, `set_channel`, `set_ham_mode` | Companion reads and projects the current settings. Changes go through the applicable settings, channel, module or licensed-mode transaction. |
| `get_ui_config_request`, `store_ui_config`, `send_input_event` | Device UI settings journal and board input owner. Input and display capabilities depend on the profile. |
| `get_device_metadata_request`, `get_device_connection_status_request` | Board/runtime capabilities and available serial, BLE and Wi-Fi status. The response omits pairing secrets. |
| `begin_edit_settings`, `commit_edit_settings` | Local bounded edit transaction. |
| `add_contact`, `remove_by_nodenum`, `set_favorite_node`, `remove_favorite_node`, `set_ignored_node`, `remove_ignored_node`, `toggle_muted_node` | NodeJournal and the existing identity-bound contact owner implement authorized edits. |
| `set_fixed_position`, `remove_fixed_position`, `set_time_only` | Existing position and clock owners. |
| `get_module_config_response` | Solicited peer responses may update descriptive remote hardware metadata. |
| `reboot_seconds`, `shutdown_seconds`, `enter_dfu_mode_request`, `ota_request` | Existing reboot, power and board-specific firmware-update owners. Bootloader and transport support determine which operations can succeed. |
| `backup_preferences`, `restore_preferences`, `remove_backup_preferences` | Local FLASH preference archive, with an identity-bound manifest and restore intent. |
| `factory_reset_config`, `nodedb_reset`, `delete_file_request` | Local operations described below. |
| `factory_reset_device` | Locally retires a provisioned fresh-domain identity, clears settings and bonds, installs a new identity and cold-boots. Requires an independent reset page, described below. |
| `lockdown_auth`, `exit_simulator` | The donor handles encrypted-storage login in PhoneAPI and drops this tag in AdminModule. The other case requires the Portduino simulator. Neither board profile exposes those facilities. |

## Authorization and payloads

Local Admin requires an admitted companion session. Managed mode denies local Admin, including reset and deletion. The new operations validate the canonical protobuf and reject an open settings edit. The firmware's RF Admin allowlists remain separate. PKI GETs use the admitted authenticated peer and bounded request correlation. The named legacy Admin channel must be enabled explicitly. Neither RF path exposes the local reset or file owners. Wi-Fi credentials, private keys and pairing secrets retain their existing redaction rules.

The pinned response map covers ten GET families including owner, config, module config, channel, canned messages, ringtone, device metadata, connection status, remote hardware pins and UI config. Config, module and channel selectors still require the expected nested response. Stock remote-hardware module-config enrollment uses a separate typed path. Output must fit the radio packet.

The donor receive switch omits the defined hardware-pin helper. Its implementation writes array entries while leaving nanopb’s repeated count at zero. It also omits the presence flag on its local pin entry. Compiling the original method with one remote and one local descriptor produced a zero-byte pin-list payload. ZT encoded both descriptors in 16 bytes using the existing compacted inventory. These lengths exclude the enclosing Admin message.

## Reset and deletion

- **NodeDB reset** removes peers using verified per-peer retirement, purges orphaned hot-node and pin metadata and schedules reboot after successful storage work. CLIENT_BASE, ROUTER and ROUTER_LATE retain favorites regardless of the requested flag. Other roles use the flag. The local identity survives. A failure interrupts the sequence and the caller can retry the remaining peers.
- **Factory config reset** commits an intent bound to the current node and public key. During startup it retires enrolled contacts through the existing owner before radio ingress opens. A verified phase change permits deletion of the named application preference keys in `factory::erasePreferences` and the configured range-test CSV. The next cold boot reloads defaults. The identity, nonce/replay authority, native BLE bonds and FLASH backup archive survive. Stock also keeps its separate /backups directory. Interrupted erasures resume. A late range-log mount keeps RF and companion ingress closed. The reset retries once per second until the external log directory is available. With a shared private volume, reset mounts it without formatting and removes the private history, peer files and owned CSV. A byte-verified erased volume already satisfies this step. An unmountable populated volume remains pending. A corrupt or mismatched intent holds startup. The Admin acknowledgment records acceptance of the request. Completion occurs across reboots.
- **File deletion** maps `/static/rangetest.csv` to the configured range-log mount. The app file must have an available filesystem owner. An absent CSV is already deleted once its configured directory is confirmed mounted. Active or queued writes return busy. The opt-in public filesystem profile also accepts regular files below its mapped public directory. Its virtual paths are checked before opening or deleting a file.

The expanded 728 KiB T096 profile places its 76 KiB filesystem at `0xe0000` and retains the separate retirement page at `0xf3000`. [T096 partition layouts](T096-FRESH-DOMAIN.md)

## Full device reset

The admitted local Admin command and, on a supported display profile, the twice-confirmed **Factory reset device** menu use the same owner. Managed mode refuses both. The firmware checks storage, radio and application quiescence and verifies the current fresh-domain binding before accepting the request. A successful acknowledgement accepts the operation. It finishes across cold boots.

| Profile | First enrollment and storage |
| --- | --- |
| XIAO with target PKI runtime | A verified, blank 4 KiB page at `0x7ef000` lies outside application, OTA, existing storage and the `0x7f0000–0x800000` settings NVS. |
| T096 with `--t096-fresh-domain --t096-device-reset` | The fresh application keeps its `0x26000–0xd0000` extent and settings keep `0xd0000–0xd4000`. The entire original 128 KiB LittleFS extent must be erased before first reset enrollment. The optional image reserves its last page at `0xf3000–0xf4000`. UF2 starts at `0xf4000`. The offline first-install checker accepts `--device-reset`. |
| Ordinary T096, or an image built without the PKI owner | Full-device reset is unavailable. Current T096 images read the first record header at `0xf3000` before activating settings. A recognizable prior enrollment, compatible partial header or read failure holds startup for the reset-capable image. The full original filesystem geometry remains available in the ordinary T096 image. |
| Legacy identity or append-history domain | The owner requires a verified fresh root and refuses either legacy NVS ledger. It never changes a legacy append partition. |

The external page has 32 append-only slots. Each records the retiring public key, the successor public key and an activation marker. Private key material remains in the existing settings/provisioning store. The implementation verifies each irreversible flash write by readback. An interrupted record can resume using its exact staged candidate and previous fresh root. After retirement, boot admits only the selected successor. A missing candidate, corrupt record, read error or restored settings belonging to a retired identity holds the radio and companion closed. A full page refuses another reset before altering settings. There is no runtime page erase.

Rebuild removes the configured range-log CSV, application preferences, hot-node and contact records, old identity and counters, both local preference archive banks, restore metadata, and native Bluetooth/GATT settings. The private reset owner discards the old cryptographic records after it has selected the new domain. It reproduces the new genesis through the existing fresh provisioning owner. A separate boot verifies the new identity and its counter/contact journals before marking it active. The final boot removes the temporary reset request. The pinned donor clears bonds and its preferences directory. Its separate backup directory can survive. ZT deliberately invalidates and deletes its local device backup on full reset.

This page enforces retirement against restoring an earlier **settings** image with the reset-aware firmware. It supplies no anti-rollback guarantee for snapshots of the same active identity's counters, for restoration of the independent page, or against installing firmware that predates these checks. Ordinary NVS deletion is logical and does not establish forensic erasure. The T096 first-install conditions and the occupied/unknown legacy storage refusal still apply.

## Validation

- `mt_only/tests/stock_admin_routing/run.py --stock <pinned-checkout>` compares 256 tags, 864 routing admissions and 288 response-hop selections on each of the two enum ABIs. It also exercises the original pin encoder, serialized CSV deletion, late mount across two cold-start recovery instances, interruption at every named erasure, wrong-domain/corrupt intents, and contact and filesystem failures.
- `mt_only/tests/run_m143_admin_snapshot.py` exercises the three added GETs with a synthetic admitted peer and an encoded response on both ABIs. Its evidence covers the snapshot and serialized response layers.
- `mt_only/scripts/test-pki-runtime.py --cases mt_only/tests/m162_node_admin_edit_tests.cpp` exercises the real Service and NodeJournal with synthetic PSA-backed peers, favorites, reboot reload, and failed writes on both ABIs. `test-node-cache.py` exercises 30 groups including encoded local Admin controls, managed-mode refusal and every partial v6 NodeJournal write.
- `zephcore/bench/run-routing-differential.py` compares the stock router and candidate with stored next-hop recovery, fresh direct-neighbor observation, last-byte collisions and the incoming relayer. NodeJournal stores one additional byte per peer when hints exist, expanding the record from 1,492 to 1,508 bytes. The loaded entry remains 288 bytes.

- `test-pki-runtime.py --cases mt_only/tests/stock_admin_routing/device_reset.cpp` exercises two successive resets, original and previous settings restores, corrupt and unreadable pages, failed initial enrollment, all 97 retirement/selection and 17 activation boundaries and each modeled settings write/removal. Both enum ABIs use the real provisioning, public-key derivation and boot verification code.
- `mt_only/tests/pki_storage_nvs/run.py --cases mt_only/tests/pki_storage_nvs/device_reset_cases.cpp --t096-fresh-domain` exercises a four-sector NOR model with the pinned Zephyr NVS and Settings implementation, real fresh provisioning, native bond subtree selection, archive deletion and a separate simulated reset page.

These checks use native fixtures and synthetic storage. Physical board reset, bond persistence and filesystem recovery require board evidence.
