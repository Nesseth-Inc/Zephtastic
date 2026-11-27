# Expanded Meshtastic handlers on Zephyr

The experiment extends nine stock modules beyond the initial 16-family comparison. The pinned Meshtastic source is `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. ZT calls the ported behavior from its existing radio, companion, display and settings paths. Both targets still run Zephyr.

| Stock module | Behavior reached from firmware |
| --- | --- |
| TextMessage | Text, detection and alert previews, plus range previews when enabled. Incoming screen wake respects role, known battery charge, mute, configured bell and external notification settings. |
| KeyVerification | The two endpoint roles build the same security-number and challenge transcripts, proof message and eight-character comparison code as the donor. Changing contact trust requires local confirmation. |
| NodeInfo | Routine and explicit transmissions use distinct timing rules and the 40% channel ceiling. The automatic floor scales from 30 minutes with congestion and uses the licensed interval when applicable. |
| Position | On-device announcements select the first channel that permits sharing. GNSS fields follow the configured position flags and sensor availability. Precision cells contribute to the stationary interval decision. Public and authenticated private Position queries share a three-minute reply window across supported roles. |
| TraceRoute | Admitted route requests and responses append node IDs and SNR observations within the payload limit. Forwarding retains ZT's authentication and channel admission rules. |
| HopScaling | An RF histogram and measured channel occupancy adapt the hop count on originated Position, NodeInfo, NeighborInfo and Telemetry broadcasts. Unicast, text and relayed traffic retain their existing hop policy. |
| Waypoint | Phone- and RF-originated waypoints enter channel- or peer-scoped local history. The owner processes expiry, locks, updates and deletion. Local notification preferences are removed before radio transmission. |
| Geofence | Enabled circles or boxes match accepted peer positions in the same scope. The first position establishes a baseline. Subsequent crossings can trigger a local alert, including a favorites-only subscription. |
| StatusLED | An explicitly selected radio RX indication pulses the board LED for 100 ms. Retriggers extend the pulse and the established LED owner arbitrates heartbeat and TX. |

The original waypoint experiment kept ten entries and tracked 16 peers until reboot. The current branch persists ten entries and tracks up to 256 node/waypoint crossing pairs. XIAO and T096 normal endpoints with native settings can restore the 128-entry HopScaling histogram from their last successful hourly checkpoint. Other profiles rebuild from RF traffic. [Warm restart qualification](../tests/stock_hop_restart/README.md)

## Earlier board measurements

Figures are bytes. The firmware was built from `a872ac5e8002e7746750ec7840374067c3ff0a6b`. Each profile used the same compiler and resolved Zephyr configuration as the preceding experiment. The last two columns give the increase relative to the 16-family build on the same profile.

| Profile | Flash used | Flash free | Static RAM used | Static RAM free | Added flash | Added static RAM |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| T096 extended onboard | 685,532 | 10,788 | 258,340 | 3,804 | 7,004 | 1,600 |
| T096 initial-release companion | 655,832 | 40,488 | 252,644 | 9,500 | 7,628 | 1,600 |
| XIAO ESP32-S3 Wi-Fi/MQTT with Store & Forward | 1,448,180 | 6,940,172 | 369,632 | 29,476 | 144 | 0 |

Both T096 images use 696,320 bytes of available application flash and 262,144 bytes of RAM. They require the fresh-domain storage layout. The old 168 KiB append journal is part of a legacy identity's nonce history, and the new application overlaps it. Existing legacy installations require a separate migration before either image can be used. A fresh identity needs zero dedicated append pages and 1,632 bytes of live PKI values at 16 contacts. [T096 storage layout and enrollment](T096-FRESH-DOMAIN.md)

In the extended T096 linker map the largest static objects include the Service at 39,696 bytes, private startup at 17,480, the system heap reservation at 16,460, the main stack reservation at 16,448, and display model storage at 9,408. The waypoint and geofence owner accounts for 1,016 bytes. Static headroom is the memory left after the linker places these reservations. Peak stack and heap use still require a running board.

XIAO static RAM in the table is internal data RAM. Its instruction RAM usage was 95,284 bytes. External RAM grew by 1,552 bytes to 2,894,416. The flash-mapped instruction segment grew by 8,304 bytes inside the image's existing alignment space. The packaged flash image grew by 144 bytes.

Both UF2 files passed nRF52840 family, application address, padding and byte-level checks against HEX and BIN.

- Extended T096 UF2 SHA-256 is `f6f082e789ad6a67eb51cf4b314462d34f1a0274447d192443f4b3c1ad7f8452`.
- Initial-release T096 UF2 SHA-256 is `60afdb7cf72bc4951b0b34f38ce156c6687ebe747923a973d7d96901a24a048a`.

## Parity and limits

The donor fixtures exercise normal and short enum representations. Each representation passed 10,368 TraceRoute mutations, 322,560 HopScaling comparisons and 290 airtime samples. TextMessage covered 11,200 combinations per representation. NodeInfo covered 410 cases and Position covered 1,729. The full key-verification exchange used synthetic peers, real SHA-256 and an explicit local confirmation. Incorrect input left the peer unverified.

Additional host checks covered signed NodeInfo suppression, Position on a sharing-enabled secondary channel, matched and unmatched Position replies, and private and licensed waypoint packets. The encoder stripped all three local notification flags before transmitting a waypoint. The pressure, expiry, identity-change and peer-removal tests exercise the state behind the UI. An ordinary display or owner-name change preserves the history. Authenticated NodeInfo RF requests can refresh a sender's 12-hour suppression interval. Unsigned shared-channel traffic records only successful replies.

The 46 Linux jobs passed on test commit `47ddffa4e3142e1451363babb75a89d3800a4e7d`, which has the same production tree as the board images. The routing differential passed 9,105 comparisons. Position precision passed 2,754 wire cases and 28 channel-setting cases per enum representation. The extended-module suite passed 32 sanitizer runs. Native tests and target linking cover source and protocol behavior. RF interoperability, physical display and notification behavior, and sustained runtime memory usage still need device measurements.

### Reproducing the expanded donor checks

Use a Meshtastic Git checkout containing the pinned commit, the repository host-toolchain prerequisites, and OpenSSL development headers and library.

```sh
for family in stock_text_verification stock_node_position stock_trace_hops stock_waypoint; do
  python3 "mt_only/tests/$family/run.py" --stock "$MESHTASTIC_FIRMWARE"
done
```

Subsequent work added the remaining registered modules, durable route hints, the keyboard and emote UI, and board-selectable accessory drivers. The upstream inventory records the current coverage and evidence. [Current module inventory](UPSTREAM-MODULE-INVENTORY.md) The firmware continues to send compliant Position relays with their original bytes and suppress over-precise relays. [Original 16-family coverage](UPSTREAM-COMPANION-MODULES.md) · [Buffer changes and PKI measurements](T096-SIZE-AND-UPSTREAM-PORTS.md)
