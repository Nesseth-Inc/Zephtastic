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

The native job pins Meshtastic firmware, the source and assertions used from its Unity suites, and the stock Python client. It runs the BME680 air-quality suite against ZT, response-hop cases against stock and ZT, and one upstream opaque-broadcast case through five supported ZT rebroadcast policies. Two generic MeshModule reply cases and ten other stock rebroadcast cases are identified in the native evidence. The endpoint and timing suites exercise stock code side by side with the port. The real Python client exchanges serialized messages with a native ZT companion through modeled USB and BLE. Inventory checks cover both the 2.8.1 baseline and the 2.8.2 target. [Pinned upstream test suites](https://github.com/meshtastic/firmware/tree/d5f16191ca28b01b5024f590f04a67f25d66f1b1/test)

The board jobs build the T096 680 and 728 KiB profiles, T114 V2, RAK4631 Mini, ordinary XIAO and three receive-only Heltec V4 variants. Each successful group uploads a JSON receipt containing its source commit. Board receipts give flash and static RAM usage and capacity. XIAO uses its unsigned application size and internal DRAM. These measurements leave peak stack, heap and physical radio behavior to the bench.

Qualification runs on trusted pushes or explicit dispatch using private self-hosted Linux runners. It uses the published XIAO verification key and produces measurements without release signing. The firmware beta retains its own hardware qualification record. [Workflow](../../.github/workflows/firmware-qualification.yml) · [Beta hardware coverage](../../README.md#01-beta-board-downloads)

## Optional USB bench check

With the designated test node already backed up and running ZT, use the stock Python package `meshtastic==2.7.11`. Supply its assigned serial port and the node ID from the backup. The default check reads the configuration handshake. Adding `--send-to` sends one direct text and verifies the peer's successful routing ACK matches the destination and request ID. The meshTestic assertions run through ZT's device selection without invoking upstream device discovery or setup. [Upstream bench assertions](https://github.com/meshtastic/meshTestic/blob/dcac7e5673005f4d8a2b1f0f6e06877b689d7519/test/config.spec.ts)

```sh
python mt_only/tests/upstream_282/usb_smoke.py --port "$ZT_SERIAL" --expect-node "$ZT_NODE"
python mt_only/tests/upstream_282/usb_smoke.py --port "$ZT_SERIAL" --expect-node "$ZT_NODE" --send-to "$ZT_PEER"
```
