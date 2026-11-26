# Operating-role parity

The fixture uses Meshtastic commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. Run `python3 mt_only/tests/stock_operating_roles/run.py --stock /path/to/meshtastic` to exercise both enum ABIs with ASan and UBSan.

| Stock role | Firmware behavior exercised |
| --- | --- |
| TRACKER | Owner marked unmessagable, hourly device telemetry, prioritized position reports, configured movement precision and post-report timed sleep |
| SENSOR | Owner marked unmessagable, hourly device telemetry and five-minute environmental measurements, sensor-report priority and timed sleep |
| TAK | Daily NodeInfo, position and device telemetry, stock position flags and byte-preserving ATAK_PLUGIN_V2 application data |
| CLIENT_HIDDEN | LOCAL_ONLY rebroadcast, maximum advertised intervals, smart position disabled and unsolicited NodeInfo suppressed |
| LOST_AND_FOUND | Five-minute periodic positions, stationary movement exception, and broadcast text with the bell character on the channel that allows position |

The comparison executes the donor’s role defaults and the production phone Admin handler, core and supplemental journals, relay queue, position worker and timed-sleep policy. It restores stored settings and tests same-role customization. The TAK app owns compression and decompression. LOST_AND_FOUND text uses the same allowed channel precision as the accompanying position packet. A changed or stale fix revokes both queued transmissions.

ZT uses the Zephyr board's wake-capable timed backend and keeps pending radio or storage work and connected consoles awake. The fixture demonstrates the role sleep interval, five-second transmit grace and wake admission. Timer-current consumption and wake behavior on a physical T096 or XIAO require hardware measurements.
