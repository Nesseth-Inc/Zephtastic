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

## What CI exercises

The native job pins Meshtastic firmware, the source and assertions used from its Unity suites, and the stock Python client. It runs the BME680 air-quality suite against ZT, response-hop cases against stock and ZT, and two upstream opaque-traffic cases. The broadcast runs through five supported rebroadcast policies. The directed case checks ID, hop budget, named relay and muted role with an opaque test payload. The native evidence lists two generic MeshModule reply cases and nine further rebroadcast cases outside this selection. The endpoint, timing, NodeInfo and Position suites exercise stock code side by side with the port. NodeInfo admission and Position reply, precision and field selection run under both enum ABIs. The real Python client exchanges serialized messages with a native ZT companion through modeled USB and BLE. Inventory checks cover both the 2.8.1 baseline and the 2.8.2 target. [Pinned upstream test suites](https://github.com/meshtastic/firmware/tree/d5f16191ca28b01b5024f590f04a67f25d66f1b1/test)

### Five module comparisons

The native receipt records a separate comparison count for each family against the pinned 2.8.2 source.

| Module | What the comparison observes |
| --- | --- |
| TrafficManagement | Simulated LoRa arrivals pass through the receive service and transmit queue. The test counts radio sends for repeated positions under different channel and traffic settings. |
| Position | The received channel's name, key and precision govern forwarding even when the relay has no fix. ZT suppresses an over-precise broadcast, forwards compliant payload bytes unchanged, and keeps the first rejected report from suppressing a later compliant one. |
| HopScaling | Random draws and channel utilization feed the stock algorithm and ZT. The packet-level checks compare how the router applies the resulting hop cap. |
| MeshBeacon | The same settings feed the stock periodic task and ZT. The comparisons cover airtime and transmit-history deferral, the next scheduled opportunity, and beacon payload selection. |
| Telemetry | The stock and ZT schedulers receive the same role, interval, congestion and regional settings. Synthetic readings exercise encoded Device and Environment announcements and replies. |

These comparisons run without a radio. Physical sensor accuracy, RF timing and multi-radio operation require separate bench measurements. Telemetry phone-queue timing also has separate coverage from its radio schedule.

The board jobs build the T096 680 and 728 KiB profiles, T114 V2, RAK4631 Mini, ordinary XIAO and three receive-only Heltec V4 variants. Each successful group uploads a JSON receipt containing its source commit. Board receipts give flash and static RAM usage and capacity. XIAO uses its unsigned application size against the usable update budget. The budget reserves 8,192 bytes for the swap trailer and 152 bytes for the longest supported P-256 signing metadata. RAM is measured in internal DRAM. These measurements leave peak stack, heap and physical radio behavior to the bench.

Qualification runs on trusted pushes or explicit dispatch using private self-hosted Linux runners. It uses the published XIAO verification key and produces measurements without release signing. The firmware beta retains its own hardware qualification record. [Workflow](../../.github/workflows/firmware-qualification.yml) · [Beta hardware coverage](../../README.md#01-beta-board-downloads)

## Optional USB bench check

With the designated test node already backed up and running ZT, use the stock Python package `meshtastic==2.7.11`. Supply its assigned serial port and the node ID from the backup. The default check reads the configuration handshake. Adding `--send-to` sends one direct text and verifies the peer's successful routing ACK matches the destination and request ID. The meshTestic assertions run through ZT's device selection without invoking upstream device discovery or setup. [Upstream bench assertions](https://github.com/meshtastic/meshTestic/blob/dcac7e5673005f4d8a2b1f0f6e06877b689d7519/test/config.spec.ts)

```sh
python mt_only/tests/upstream_282/usb_smoke.py --port "$ZT_SERIAL" --expect-node "$ZT_NODE"
python mt_only/tests/upstream_282/usb_smoke.py --port "$ZT_SERIAL" --expect-node "$ZT_NODE" --send-to "$ZT_PEER"
```
