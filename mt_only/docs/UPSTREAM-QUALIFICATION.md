# Keeping Zephtastic close to Meshtastic 2.8.2

Work through the modules in order of how many boards, peers and clients can observe a disagreement. Most checks use the same packet or setting as input to both implementations. Where a port changes upstream behavior intentionally, record the reason alongside the test.

| Priority | Upstream owner | Behavior to keep aligned |
| --- | --- | --- |
| 1 | RadioInterface, FloodingRouter and NextHopRouter | A timing or filtering change affects every relay. Compare rebroadcast decisions, duplicate delays and signal strength across router and client roles. The SX126x LONG_FAST case uses a 44 ms slot and a 704 ms minimum ROUTER_LATE delay. |
| 2 | ReliableRouter and RoutingModule | Changes to ACK eligibility or reply hops affect DM delivery, retries and channel load. Check directed text, compressed text, other application packets and terminal ACKs. |
| 3 | AdminModule, PhoneAPI and role defaults | Clients must read back the settings an ACK said were saved. Exercise the actual stock client, local DeviceConfig writes, role transitions and restart. |
| 4 | NodeDB, NodeInfoModule and TypeConversions | The node list is shared by discovery, routing and the phone. Compare the fields reported to clients after a record has been persisted and reloaded, including verified identity metadata. |
| 5 | TrafficManagementModule and PositionModule | Check suppression, position precision and the resulting on-air packet count. ZT preserves authenticated packet bytes and suppresses positions above the channel precision limit. |
| 6 | HopScalingModule, MeshBeaconModule and TelemetryModule | Compare packet fields and scheduling under load. Start with pure calculations, then cover board-dependent sensor and radio behavior on hardware. |
| 7 | TextMessage, KeyVerification, TraceRoute, NeighborInfo, Store & Forward and MQTT | Compare decisions that affect another node or a phone first. Exercise wire fields, directed replies, local trust confirmation, replay scope and broker admission. |
| 8 | Waypoint, Geofence, RangeTest, StatusMessage and ATAK | Compare updates, notifications, congestion admission and preserved data bytes. |
| 9 | Local reports, user interface and optional peripherals | Exercise phone queues, local commands, games, sample conversions and the boundaries of the selected board profiles. |

## What CI exercises

The native job pins Meshtastic firmware, the source and assertions used from its Unity suites, and the stock Python client. It runs the BME680 air-quality suite, the 16 response-hop assertions and the ordinary rebroadcast cases. The receipt names each original assertion and the directed case exercised through ZT's packet path. The endpoint, timing, NodeInfo and Position suites exercise stock code side by side with the port. NodeInfo admission and Position reply, precision and field selection run under both enum ABIs. The real Python client exchanges serialized messages with a native ZT companion through modeled USB and BLE. Inventory checks cover both the 2.8.1 baseline and the 2.8.2 target. The persistent broadcast-history regression suite retains its 2.8.1 clock reference and runs in both enum modes. [Pinned upstream test suites](https://github.com/meshtastic/firmware/tree/d5f16191ca28b01b5024f590f04a67f25d66f1b1/test)

### Five module comparisons

The native receipt records a separate comparison count for each family against the pinned 2.8.2 source.

| Module | What the comparison observes |
| --- | --- |
| TrafficManagement | Simulated LoRa arrivals pass through the receive service and transmit queue. The test counts radio sends at rate-window boundaries and checks how position deduplication interacts with the per-sender limit. |
| Position | The received channel's name, key and precision govern forwarding even when the relay has no fix. ZT suppresses an over-precise broadcast, forwards compliant payload bytes unchanged, and keeps a privacy-rejected report out of the position deduplication window. The per-sender packet limit still applies. |
| HopScaling | Both implementations receive the same node population, hash seed and channel utilization. The packet-level checks compare how the router applies the resulting hop cap. |
| MeshBeacon | The same settings feed the stock periodic task and ZT. The comparisons cover airtime and transmit-history deferral, the next scheduled opportunity, and beacon payload selection. |
| Telemetry | The stock and ZT schedulers receive the same role, interval, congestion and regional settings. Synthetic readings exercise encoded Device and Environment announcements and replies. |

These comparisons run without a radio. Physical sensor accuracy, RF timing and multi-radio operation require separate bench measurements. The reports job below also checks unsolicited local Position and idle-phone Telemetry. XIAO and T096 normal endpoints with `CONFIG_MT_HOP_HISTORY` restore the last successful hourly HopScaling checkpoint. The initial cap is seven until the first hourly roll. Profiles without the feature and devices without a readable checkpoint rebuild from RF traffic. [Warm restart qualification](../tests/stock_hop_restart/README.md)

The board jobs build the T096 680 and 728 KiB profiles, T114 V2, RAK4631 Mini, ordinary XIAO and three receive-only Heltec V4 variants. Each successful group uploads a JSON receipt containing its source commit. Board receipts give flash and static RAM usage and capacity. XIAO uses its unsigned application size against the usable update budget. The budget reserves 8,192 bytes for the swap trailer and 152 bytes for the longest supported P-256 signing metadata. RAM is measured in internal DRAM. These measurements leave peak stack, heap and physical radio behavior to the bench.

Qualification runs on trusted pushes or explicit dispatch using private self-hosted Linux runners. It uses the published XIAO verification key and produces measurements without release signing. The firmware beta retains its own hardware qualification record. [Workflow](../../.github/workflows/firmware-qualification.yml) · [Beta hardware coverage](../../README.md#01-beta-board-downloads)

## Remaining module checks

The upstream inventory has 44 registrations in 2.8.2, including two conditional shell registrations. Ten other rows cover companion or support code. CI assigns each row to a suite. Each new module receipt identifies the donor commit, names the behavior exercised, and distinguishes direct comparisons from ZT behavior tests, source checks and profile exclusions. [Module inventory](UPSTREAM-MODULE-INVENTORY.md)

| Private runner job | Coverage |
| --- | --- |
| Wire | Text and key verification, relay path and neighbor reports, store-and-forward and MQTT decisions, waypoints, geofences, range and status reports, and transparent ATAK payloads. |
| Reports | Air quality, health, power, host metrics and unit conversions, with local phone reporting checked separately from RF. |
| Peripherals | Device input and commands, optional example modules, simulated GPIO, notifications, audio, serial, passive counts, displays and games. |
| Core | Beacon listener selection, retention of offer fields, and transmit or defer decisions on a channel change. The two stock shell registrations are checked against their Portduino and dedicated test-peer build conditions. |

The reports job checks the scheduled DeviceMetrics and LocalStats pair on the same eligible idle session. It checks the unsolicited 32-bit Position packet on the local USB console and encrypted BLE sessions, including blocked and disconnected clients. This producer has no RF, MQTT or browser output. In the full companion the ordinary startup slots are 45 seconds for MeshBeacon, 60 seconds for Position and 90 seconds for DeviceTelemetry. Trackers start Position immediately, leaving a 75-second DeviceTelemetry slot.

The core job compares the Git objects for the upstream telemetry sensor directory. All 46 pre-existing C++ units are byte-identical between 2.8.1 and 2.8.2. The two added units support the RAK2560 hub. The run also exercises selected Zephyr sensor workers with simulated I²C and clock inputs and the pinned Maxim optical algorithm. The RAK4631 Mini and the RAK2560 hub are different hardware. The checks do not measure calibration, optical contact, antenna response, or RF retune timing. [Sensor inventory](UPSTREAM-TELEMETRY-SENSORS.md)

## Five additional behavioral comparisons

| Upstream code | What the private runners compare |
| --- | --- |
| FloodingRouter | Licensed and ordinary relay decisions, repeated packets and five rebroadcast policies. Ten tests reuse upstream assertions. The directed known-peer case uses a recipient-verified synthetic packet and checks what reaches the radio and phone. The deprecated stock REPEATER mode has its own recorded exclusion. |
| MeshModule | Stock reply envelopes and local companion module replies, including the request ID, addressing, channel, reply hop limit, acknowledgment flag and preservation of an existing priority. |
| MeshBeaconTxHook | Channel selection for targeted beacons, a fresh contention delay after retuning, and restoration of the home settings. A synthetic radio records transmission order. Antenna timing and carrier detection on physical hardware remain bench work. |
| GamesModule | Stock menu and game transitions compared with the ZT host. The checks cover the score-clear dialog, button actions and idle timeouts. |
| Tetris | Independently compiled stock and ZT game cores receive identical pieces and controls. The checks compare board cells, the current and next piece, ghost position, row clears, levels, wall kicks and game over. |

## Optional USB bench check

With the designated test node already backed up and running ZT, use the stock Python package `meshtastic==2.7.11`. Supply its assigned serial port and the node ID from the backup. The default check reads the configuration handshake. Adding `--send-to` sends one direct text and verifies the peer's successful routing ACK matches the destination and request ID. The meshTestic assertions run through ZT's device selection without invoking upstream device discovery or setup. [Upstream bench assertions](https://github.com/meshtastic/meshTestic/blob/dcac7e5673005f4d8a2b1f0f6e06877b689d7519/test/config.spec.ts)

```sh
python mt_only/tests/upstream_282/usb_smoke.py --port "$ZT_SERIAL" --expect-node "$ZT_NODE"
python mt_only/tests/upstream_282/usb_smoke.py --port "$ZT_SERIAL" --expect-node "$ZT_NODE" --send-to "$ZT_PEER"
```
