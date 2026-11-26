# Signed XIAO BLE update qualification

The optional signed two-slot XIAO profile registers the Meshtastic ESP updater after an authorized OTA_BLE request with the candidate’s 32-byte SHA-256. The ordinary phone connection closes before the device advertises the update UUID at its existing Bluetooth identity. The phone’s configured encrypted pairing policy applies again to that connection. A disabled Bluetooth policy refuses the carrier.

The host test compiles the production GATT callbacks, ordinary phone security callbacks and update loop. Its storage double models a terminal rejection and records bytes. The shared Session and bootloader own signature verification and trial selection. Supplying the stock checkouts also compiles the donor stream parser and compares its responses to the production carrier using the same accepted writes.

The Android reconnect case exercises an initial attempt to reuse an old link key in NO_PIN mode. It verifies that registration for notifications and firmware writes are denied before fresh encryption, then retries registration and completes the update on the same connection.

The native Settings cases run a concurrent Bluetooth CCC save through the production storage coordinator. They check that an already admitted epoch retains its next Write Command through a healthy lease, that storage and notifications resume in order, and that disconnect, encryption downgrade, storage failure or a nine-second wait aborts the transfer. A first write during a lease cannot establish an epoch.

A valid START must arrive within two minutes of opening the loader. An admitted connection expires after 45 seconds of inactivity. START sets one absolute transfer deadline from the accepted image length. It allows 45 seconds plus ten seconds for each 20-byte write the image would require at ATT MTU 23. The pinned stock Android client allows ten seconds per ACK. Repeated commands, empty lines and later chunks cannot extend the deadline. For a 1,599,050-byte image the absolute limit is 9 days, 6 hours, 6 minutes and 15 seconds. The 20-byte virtual-time fixture takes about 1 hour and 53 minutes at an 85 ms cadence. It also checks idle expiry, steady one-byte trickle and repeated pre-START commands.

```sh
python3 mt_only/tests/stock_signed_ble_ota/run.py --stock-loader "$STOCK_OTA" --android "$ANDROID" --output "$RESULTS/normal"
python3 mt_only/tests/stock_signed_ble_ota/run.py --stock-loader "$STOCK_OTA" --android "$ANDROID" --short-enums --output "$RESULTS/short"
```

## Physical test on an authorized disposable XIAO

1. On a Linux Bluetooth central, identify the original phone connection from its node number, public fingerprint and board model. Confirm ordinary encrypted phone operations and record its address and boot count. Check that discovery has no OTA service.
2. Use the existing trusted local Admin session to arm OTA_BLE for the exact signed candidate SHA-256. Observe the normal acknowledgment and disconnect. Scan for service `4fafc201-1fb5-459e-8fcc-c5c9c331914b` at the same device address. Complete the configured link-security requirement, then subscribe to `62ec0272-3ec5-11eb-b378-0242ac130003`.
3. Send `OTA <decimal bytes> <64 hexadecimal digest>\n` to `62ec0272-3ec5-11eb-b378-0242ac130005` using writes with response. Split the line at the negotiated write payload. Require separate `ERASING\n` and `OK\n` notifications. A VERSION diagnostic needs an ATT payload large enough for the complete version line.
4. Use one write without response per image chunk, bounded by 512 bytes and the negotiated ATT payload. Wait for `ACK\n` after each intermediate chunk and the explicit `OK\n` after the final chunk. After the normal reboot, verify the trial image, confirmation, incremented boot count, enrolled identity, retained settings and disappearance of the OTA UUID.
5. In separately armed negative trials check the wrong announced digest, a signed-image validation failure and disconnect partway through transfer. Verify the original boot selection and ordinary phone identity. After a transfer has started, a replacement connection must arm an entirely new attempt.

The asynchronous rejection cases hold the notification after Zephyr accepts it, simulating a controller that transmits later. They check the full validation error at MTU 517, the complete stock `ERR Hash Mismatch` classification at MTU 23, and an invalid command that terminates the byte-stream parser. The update controller keeps the same secured link alive for transmit completion, with a two-second bound. The tests also exercise disconnect/reconnect before completion and a missing completion. None of those error cases selects the candidate image. A Zephyr transmit completion does not establish that the client application displayed the error.

The authenticated OTA peer requests the stock loader's 15 ms interval, zero
peripheral latency and four-second supervision timeout on its first reply.
The carrier uses Zephyr's per-connection API, which replaces the pending
peripheral GAP preference before its timer fires or submits an update afterward.
The central selects the effective values. The tests check the pinned loader's
request, the peer and security epoch, exactly one request over a full transfer,
and continued verified transfer when that request is refused. Ordinary PhoneAPI
connections keep the board's existing GAP preferences.

The terminal-error fixture can complete the controller callback immediately and
delay the simulated central's receipt by 55 ms. It checks that the secured
service survives for the same two-second window as a terminal OK, unless the
peer leaves first. Teardown observes the disconnect callback for the same
connection epoch before unregistering. A missing callback is bounded at five
seconds. The fixture also covers a delayed disconnect callback after 75 ms and
preserves the inactive image on every validation failure.
