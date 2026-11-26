# Operating-role parity

The default fixture uses Meshtastic commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. Run `python3 mt_only/tests/stock_operating_roles/run.py --stock /path/to/meshtastic` to exercise both enum ABIs with ASan and UBSan.

| Stock role | Firmware behavior exercised |
| --- | --- |
| TRACKER | Owner marked unmessagable, hourly device telemetry, prioritized position reports, configured movement precision and post-report timed sleep |
| SENSOR | Owner marked unmessagable, hourly device telemetry and five-minute environmental measurements, sensor-report priority and timed sleep |
| TAK | Daily NodeInfo, position and device telemetry, stock position flags and byte-preserving ATAK_PLUGIN_V2 application data |
| CLIENT_HIDDEN | LOCAL_ONLY rebroadcast, maximum advertised intervals, smart position disabled and unsolicited NodeInfo suppressed |
| LOST_AND_FOUND | Five-minute periodic positions, stationary movement exception, and broadcast text with the bell character on the channel that allows position |

The comparison executes the donor’s role defaults and the production phone Admin handler, core and supplemental journals, relay queue, position worker and timed-sleep policy. It restores stored settings and tests same-role customization. The TAK app owns compression and decompression. LOST_AND_FOUND text uses the same allowed channel precision as the accompanying position packet. A changed or stale fix revokes both queued transmissions.

ZT uses the Zephyr board's wake-capable timed backend and keeps pending radio or storage work and connected consoles awake. The fixture demonstrates the role sleep interval, five-second transmit grace and wake admission. Timer-current consumption and wake behavior on a physical T096 or XIAO require hardware measurements.


With a Meshtastic 2.8.2 checkout or source archive, run `python3 mt_only/tests/stock_operating_roles/run.py --release 2.8.2 --stock /path/to/meshtastic`. The fixture verifies the donor files against commit `d5f16191ca28b01b5024f590f04a67f25d66f1b1` and executes the stock Admin device-config case and NodeDB role defaults. A simulated phone sends simultaneous role, timezone, rebroadcast and NodeInfo changes for ROUTER_LATE and TAK_TRACKER, then reads the response and reloads both settings journals. Subsequent same-role requests exercise customization and the one-hour NodeInfo minimum.

The 2.8.2 phone fixture also reloads the NodeDB and compares peer signing and manual verification fields against stock `TypeConversions::ConvertToNodeInfoThin` during ordinary and nodes-only handshakes. It covers a previously verified signer, a manually verified key, a known unsigned key and an owner-replaced signing key. Passing `--output results.json` records the actual number of DeviceConfig assertions and restored peer comparisons for each enum ABI.
