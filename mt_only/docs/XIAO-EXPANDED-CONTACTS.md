# XIAO private contact storage

The opt-in signed XIAO ESP32-S3 profile, `mt-zephyr-xiao-198`, supports 198 explicitly pinned private contacts. Installing the image leaves the existing 16-key store in use until the USB owner activates the expansion. Both profiles support ordinary local PhoneAPI `Admin.add_contact`. After expansion, a directory entry or received NodeInfo grants no private-message authority. Before activation, the ordinary profile can still persist a signature-verified, channel-authenticated NodeInfo under its existing admission policy.

## Activation and readback

Verify a complete recovery image for this device before changing the signer. Use the [release and recovery procedure](XIAO-UPDATES.md#opt-in-to-198-trusted-private-contacts). Confirm the expanded image under its dedicated MCUboot signing authority. Open an ordinary local USB PhoneAPI session on the enrolled node. Send these commands as canonical PhoneAPI text packets addressed to the local node, with the sender field unset, zero hops, no ACK request and PKI encryption off. The local USB handler consumes the command. Match the response's `request_id` to the request.

```text
zt_pki_contacts_expand_v1 status
zt_pki_contacts_expand_v1 <local node, eight lowercase hex digits> <local public key, 64 lowercase hex digits>
```

The activation response reports the result, retained count, and whether a reboot was scheduled. After reboot, check `status` for `capacity=198`, `expanded=1` and the retained count. The `through` field reports the last reserved private transmit nonce. Record it together with the key inventory before and after a recovery exercise. The existing fresh PKI domain can contain zero through sixteen pins. The import verifies both legacy contact records and the reserved flash area. An occupied or unrecognized staging area causes a refusal without erasing it.

An additional explicit pin uses the ordinary contact workflow. Full capacity rejects a 199th simultaneous unique pin. Removing a pin frees one slot. Replacing an existing public key requires an explicit owner action. Factory configuration reset removes private pins. Full device reset also erases the owned expanded banks while retiring the old identity.

## Storage and recovery

The expanded partition occupies `[0x480000, 0x488000)`. Four 8 KiB banks rotate. Each bank holds up to 198 public node/key pairs at 36 bytes each. A mirrored header identifies the local identity and domain in each erase sector. A write prepares and reads back the full candidate, replaces one of the two existing NVS contact selectors, verifies that selector, and writes a commit witness before acknowledging the change. Boot validates both selectors and their banks. An acknowledged witness newer than the restored NVS forbids reopening stale contacts.

The change retains the global persistent transmit nonce counter and its original reservation rules. Four hot cryptographic sessions and four bounded, volatile receive replay windows serve the directory. Reboot or eviction drops a receive window as in the ordinary fresh-domain profile. Changes to membership require no new private key and do not reset the transmit counter.

A signed ordinary update uses a different signing authority. A raw downgrade after activation also leaves an unreadable contact selector in the ordinary firmware. Switching profiles requires verified device recovery. Restoring a stale flash snapshot under the same local private identity after later transmissions could reuse nonce reservations. Recover the latest consistent domain and reservation state or retire that identity and enroll a fresh one before transmitting. Restoring both flash regions to the same earlier point is beyond the software rollback witness.
