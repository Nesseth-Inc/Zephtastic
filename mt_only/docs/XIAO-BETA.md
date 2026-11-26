# XIAO 0.1-beta · Meshtastic 2.8.2

This beta is for a spare **Seeed Studio XIAO ESP32-S3 with Wio-SX1262, 8 MiB flash, ordinary 16-contact profile**. Its compatibility target is stock Meshtastic `2.8.2.d5f1619`. The 198-contact profile, T096 and other boards have separate qualification histories. The ordinary release images use the retained 0.1-beta signing authority. [Build instructions](../../README.md#build-the-xiao-from-source)

## Installing and recovering

1. Save and verify a full 8 MiB recovery copy for the specific board before a factory flash. Keep it private. A configuration export is useful for preferences and channels. Use Android's **Security → Backup Keys** separately if you want to retain the node identity. [Installation and recovery](XIAO-UPDATES.md) · [Identity migration](STOCK-IDENTITY-IMPORT.md)
2. Build the ordinary profile. Anyone making a series of signed updates must retain its signing authority and use the same public key in the installed bootloader. A test key from another build cannot authorize your next update. [Signing and update formats](XIAO-UPDATES.md)
3. After a migration, reboot and read the radio's node number, public key, names, channel and LoRa settings. Check the region, hop limit and transmit setting locally before enabling RF. Test a direct message with a trusted peer and verify receipt at both ends. The migration guide explains the restrictions on sending to confidential shared channels under a stock-imported identity. [Android migration procedure](STOCK-IDENTITY-IMPORT.md)

## What was tested

The physical migration and signed update used the preceding ordinary preview. The mixed-transport run used the same source with memory instrumentation. The profile restore used the same firmware source with a disposable validation signer.

| Check | Result and scope |
| --- | --- |
| Stock Android migration | Android 2.8.4 backed up an identity from a physical stock 2.8.2 XIAO and restored it to a freshly flashed ordinary preview. The public key and node number matched after reboot. The earlier preview restored the secondary channel and key, but dropped the owner fields, region and hop limit. |
| Android profile restore | Stock Android 2.8.4 imported the profile over BLE on a physical XIAO. An independent USB read after reboot passed 18 checks, including both names, Unmessageable, the private secondary channel and key, US region, hop limit 1 and unchanged node identity. Native tests also cover the USB and BLE owner and radio handlers. A read-only copy of the device ID, model and public key can accompany a local owner edit. Attempts to replace those fields are rejected. |
| Android BLE update | The stock app used the existing bond, flashed the next signed ordinary preview and displayed “Update Successful!”. Two complete device reads confirmed the new image in the primary slot and the preceding signed image in the secondary slot. The migrated key and channel survived. |
| Encrypted messaging | The stock Android app communicated through Unraid's physical Bluetooth adapter to a real XIAO. A stock Nordic peer decrypted the outbound text over LoRa. It sent return texts that arrived in Android's private conversation on the instrumented build. Packet IDs and message digests matched. The phone was an Android emulator. |
| Mixed BLE and Wi-Fi use | The instrumented sibling ran for 4 h 48 min. Five saved-bond BLE reconnect tests passed. Chromium completed 57 of 58 scheduled authenticated sessions. One unplanned XIAO restart changed the boot number and caused the first post-restart browser check to fail. Android reconnected automatically. |
| Routing | Native tests exercised stock 2.8.1 and 2.8.2 traceroute methods and the stock client. Traceroute replies preserved hop and signal data, including fractional readings supplied by stock peers. |

## Known issues

- **Unexpected restart.** A diagnostic XIAO restarted once after about three hours of mixed BLE and Wi-Fi traffic. USB remained enumerated. The build retained the boot number but no reset cause. This limits any unattended-uptime claim.
- **Occasional private-send refusal.** One Android-originated DM on the diagnostic build received local `NO_INTERFACE` and never reached its peer. The next scheduled outgoing message succeeded. Check Android's delivery state and the recipient's history before retrying.
- **Two-hop route learning.** After a successful two-hop traceroute, stock reused the known relay for its next packet. ZT left its next hop unspecified in the same native test. A physical three-node test with the direct path blocked is still outstanding.
- **Hardware coverage.** Stock Android handset models, iOS, independent power cuts during an update, accessories and other board profiles require separate physical tests. The update-swap interruption results on this source line used emulation.

When reporting a problem, include the board, profile, source revision, phone app version, transport, whether the node rebooted, and the error visible to the sender. Keep key material, recovery images, channel URLs and private message contents out of reports.
