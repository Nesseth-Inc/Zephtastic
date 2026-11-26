# T096 companion fit and Meshtastic handler coverage

The next comparison adds nine stock modules and records the resulting T096 and XIAO headroom. [Expanded Meshtastic handlers on Zephyr](EXPANDED-STOCK-MODULES.md)

The full initial-release companion can link on a freshly provisioned Heltec T096. The firmware runs Zephyr and continues to use ZT's radio, BLE, filesystem and board drivers. Ported Meshtastic handlers receive real ZT settings, observations and queue results.

## What recovered space

The earlier full companion demanded 650,956 bytes of flash and 297,848 bytes of static RAM. Its legacy application slot held 524,288 bytes and the nRF52840 has 262,144 bytes of RAM. A controlled build changed only routing and BLE sizing. It used 651,068 flash bytes and 258,084 RAM bytes, a saving of 39,764 RAM bytes. The flash deficit on that layout was still 126,780 bytes.

- Routing keeps all 64 opaque packet history entries. On the T096, each entry keeps its existing 64-bit payload fingerprint in place of a 239-byte diagnostic copy of the ciphertext. Routing deduplication still uses the sender and packet ID. The transmit queue keeps the complete frame. A byte-by-byte difference location for opaque duplicate diagnostics is available only on the other profiles.
- T096 BLE keeps eight outbound packets and negotiates up to a 247-byte ATT MTU. The ACL buffers hold 251 bytes and the controller data-length setting stays at 27 bytes. The queue applies backpressure when full. All 29 prepare-write buffers remain available. The host harness reads and writes 512-byte companion messages at both MTU 23 and MTU 247. Lower queue depth reduces the burst a phone can leave unread.
- A new identity uses the shared NVS nonce counter and contact snapshots. Its application can occupy the former 168 KiB journal. Old identities use that append history to fence earlier nonces even after switching their active counter. The fresh-domain image overlaps that history and is unsuitable for installation over a legacy identity. [Layout, first enrollment, mounted-NVS measurements and limitations](T096-FRESH-DOMAIN.md)

## Module ports since the first 16-family pass

The donor is Meshtastic firmware commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. Existing [family coverage and relay policies](UPSTREAM-COMPANION-MODULES.md) provide the starting point.

| Family | Runtime behavior exercised in this pass |
| --- | --- |
| MQTT | MapReport generation and coordinate precision, channel subscription selection, and dispatch between the broker and phone proxy. |
| Neighbor Info and Traffic Management | Neighbor collection, bounded retention, expiry and announcement scheduling. Cached NodeInfo replies respect the configured hop limit, consent, throttling and queue admission. Setting the direct-response hop limit to zero disables the replies. |
| Store & Forward | Addressed legacy text history requests, archive selection and idle heartbeat/active replay scheduling. Replays keep each record's channel. The transfer finishes at the announced cap even when more history is available. The server needs XIAO PSRAM. |
| Mesh Beacon | The sender checks the regional duty budget before recording an announcement. The transmit path checks channel occupancy at dispatch. The listener filters the configured ports. |
| Range Test | The log worker computes stock approximate distance from available peer and receiver coordinates. It handles the antimeridian. ZT owns CSV quoting, missing fields and filesystem writes. |
| Serial | Stock WPL and GGA position sentences, CalTopo callsign output, and WS85 weather averaging/reporting. A dedicated UART and matching board wiring are required for hardware operation. |
| Canned Messages and Audio | Wraparound selector and outgoing text through the configured destination/channel. Push-to-talk and MAX-priority audio submission use the real bounded transmit queue. |
| Paxcounter | Solicited replies preserve the channel, request ID, hop and ACK policy on channel encryption or authenticated PKI. The observer must have fresh data. The bounded duplicate cache retains eight successful requests for ten minutes and applies a five-second per-source minimum. |
| Detection Sensor and External Notifications | GPIO observation and heartbeat scheduling, local dismissal, PWM/I²S song completion and tick rollover. |
| Telemetry | Air Quality and Health reports carry the available UTC timestamp in replies and announcements. Cached remote observations retain the original wire time. Freshness checks use local uptime. Periodic reports use the stock role and congestion intervals. Health also applies regional throttling. Phone-only reports enter each transport only when its outbound queue is idle. |
| Status Message, Remote Hardware and Ambient Lighting | Previously ported handlers remain in use. The actual output depends on the board's exported GPIO/PWM devices. |

The 16 families have production bindings. Full stock feature parity also requires choices outside these handlers. The T096 and XIAO release overlays declare no air-quality sensor. Sensor-specific administration, a free-text keyboard/emote UI, and durable NodeDB next-hop hints need additional drivers, UI or persistent data. ZT supports in-memory learned routes. It also uses its own BLE, RF, queue and power-management lifecycles.

## Target measurements

All figures are bytes. The artifacts were built from firmware commit `f75af9536ffdb5623b2b6039ed4da06ef23bd69d` with the same pinned compilers as the earlier comparison. Static RAM headroom is measured at link time. Dynamic heap, stack peaks, RF and board behavior require device measurements.

| Profile | Flash used | Flash free | Static RAM used | Static RAM free |
| --- | ---: | ---: | ---: | ---: |
| T096 extended onboard, fresh identity | 678,528 | 17,792 | 256,740 | 5,404 |
| T096 initial-release companion, fresh identity | 648,204 | 48,116 | 251,044 | 11,100 |
| XIAO ESP32-S3, Wi-Fi/MQTT/PSRAM Store & Forward | 1,448,036 | 6,940,316 | 369,632 | 29,476 |

Both T096 profiles have 696,320 bytes of application flash and 262,144 bytes of RAM. The extended onboard build includes TAK, GPIO handlers and passive BLE Paxcounter. GPS, display, RF admin, BLE companion and normal-TX PKI are enabled in both. Boards with no dedicated UART, audio device or sensor still lack the corresponding hardware observations.

XIAO RAM in the table is internal data RAM. Its linker also used 95,284 bytes of instruction RAM and 2,892,864 bytes of external RAM. The flash budget is the 8,388,352-byte application image limit. The previous branch measurement was 1,448,052 flash bytes and 369,632 internal data RAM bytes.

The T096 UF2 block addresses and nRF52840 family ID were verified. Every byte represented by the linker HEX matched the UF2 and BIN. Alignment gaps and the final 256-byte UF2 block padding were checked separately. All UF2 blocks end before the shared settings partition.

The SHA-256 of the extended T096 UF2 is `b8da20cd61b93c3697dd0a4ab924fda12dcd8ecf29b17f1d61f61cc036835f6f`. The initial-release UF2 is `175f9f764a2add919b3dee258712700d1d47f658639c2e77762c8fdd66f78e7d`.

## Parity evidence

The host checks compare production handlers with code from the pinned Meshtastic checkout. Normal and short enum builds cover the embedded ABI difference. The routing differential passed 9,105 comparisons. Position passed 2,754 encoded combinations and 28 channel-setting cases in each ABI. The extended-module suite passed 32 sanitized executions.

All 26 Linux jobs passed. The Linux suite covers stock gateway and sensor behavior, neighbor and companion settings, Store & Forward, native and fresh NVS, queue sizes, routing core, canned input, background senders, Audio and Paxcounter. The dedicated Paxcounter test uses real PSA crypto to verify authenticated replies, channel handling, duplicate and pressure behavior. No board was flashed.
