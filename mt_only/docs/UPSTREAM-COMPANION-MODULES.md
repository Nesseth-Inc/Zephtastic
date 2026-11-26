# Upstream handlers on the ZT companion and repeater

This experiment runs selected Meshtastic 2.8.1 handlers through the existing Zephyr firmware. It covers all 16 families in the companion/repeater map. Status Message ports the complete stock implementation file with a platform adapter, following the earlier PositionPrecision experiment. The other 15 families port the listed paths from larger stock modules. A successful row means the named behavior is wired and tested. Complete stock applications, every operating mode and physical peripherals have a wider qualification scope. [module inventory](UPSTREAM-MODULE-INVENTORY.md) · [Pinned Meshtastic source](https://github.com/meshtastic/firmware/tree/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c) · [PositionPrecision experiment](UPSTREAM-POSITION-PRECISION.md)

| Family | Stock behavior running in ZT | Platform or scope boundary |
| --- | --- | --- |
| MQTT | Selects the packet for uplink and filters ports by configured broker. Selects the regional root. | ZT owns broker/client proxy transport, downlink admission, channel permissions and remote opt-out. |
| Audio / Codec2 | Frames audio packets and dispatches received frames to the decoder. | ZT owns bounded buffers, Codec2 mode mapping, I²S and push-to-talk. Requires an audio-capable build and board. |
| Ambient Lighting | Applies enabled RGB values and refreshes output from the scheduled handler. | Zephyr drives the board’s declared PWM outputs. |
| Paxcounter | Builds and sends the Wi-Fi/BLE count and uptime report when a fresh observation is available. | Zephyr owns radio observation and scheduling. The XIAO Wi-Fi observer is independently gated. |
| Canned Messages / Input | Splits the configured list, updates stored message text through the admin path and constructs outgoing text. | Input, UI selection, journal and send admission stay in ZT. |
| External Notifications | Classifies incoming alerts, picks enabled outputs and arms the repeat interval. | Zephyr owns GPIO, PWM and I²S. Each output requires board support. |
| Store & Forward | Dispatches server requests, starts history transfers, sends statistics and observes server heartbeats on the client. | ZT owns the XIAO PSRAM archive and bounded queues. History stays on the channel that opened it. A full control queue permits a later retry. |
| Range Test | Schedules and sequences range announcements and selects received packets for logging. | ZT owns queued radio submission and optional filesystem logging. |
| Telemetry | Combines fresh Air Quality and Health readings and selects their telemetry reply variants. | Zephyr supplies sensor channels. This row covers the two telemetry groups in the coverage map. |
| Remote Hardware | Dispatches GPIO reads, writes and watches, and schedules watch updates. | ZT enforces the exported pin table, permissions and deduplication before touching hardware. |
| Neighbor Info | Observes direct packets and announcements, sends neighbor snapshots and forwards eligible announcements unchanged. | ZT owns the observation cache and radio admission. Relay eligibility is detailed below. |
| Detection Sensor | Evaluates configured levels and edges and formats detection messages. | A valid exported pin, local configuration and transmit permission are required. |
| Status Message | Runs the scheduled sender and receive handler from the ported implementation file. | The adapter supplies the real status setting, primary-channel send and NodeDB receiver. |
| Traffic Management / NodeInfo | Applies rate limits, unknown-sender and duplicate-position rules, and checks Position precision before relay. | ZT owns the cache, protected NodeInfo paths and queue admission. |
| Mesh Beacon | Selects text, radio offers or combined packets, including the legacy split, and handles received offers. | ZT owns target selection, radio excursions and queueing. |
| Serial | Dispatches SIMPLE/TEXTMSG receive and echo behavior and decodes positions for NMEA and CalTopo output. | Zephyr owns the UART. PROTO and WS85 continue through their existing ZT engines. |

## Relay behavior

When Traffic Management applies a Position limit, a relay forwards a compliant packet with its original payload bytes. It suppresses a packet that declares more precision than allowed or carries finer coordinates than its declaration. It uses the receiving channel even without a local GNSS fix. For slots with the same on-air identity, the strictest applicable limit wins. The stock exemptions for licensed operation, an unset limit and private channels apply under the default build policy. ZT stores expanded primary keys, so a raw primary key indistinguishable from a public alias receives the same limit as that alias.

Neighbor Info announcements omit `last_sent_by_id`. A direct observation uses the radio source. For a relayed observation, a legacy sender field must agree with the relay byte, or the NodeDB must resolve that byte to a unique fresh direct neighbor. Ambiguous relays create no edge. Enabled RF relays preserve the payload bytes and use normal signature, hop and routing admission. MQTT-originated Neighbor Info and requests remain ineligible for this path.

The 2.8.2 Traffic Management default suppresses repeated positions for six hours. Stationary transmitters wait at least six hours and six minutes. An explicitly saved interval remains effective, including zero.

RF-origin encrypted MQTT observations between two other nodes use an independent eight-entry sender and packet-ID window. They can publish with no RF relay slot or retransmission. The board still requires encrypted MQTT and channel uplink consent, honors ignored and known-only peers, and excludes an ambiguous configured hash-zero channel. An unsigned decoded MQTT NodeInfo and a local phone NodeInfo show the radio sender's node ID. The encrypted radio envelope and a signed broker publication retain their original bytes.

Changes to hot NodeDB identities remain dirty until a quiescent checkpoint or ordinary graceful restart. ZT’s journal keeps its ten-minute normal write budget, alternate verified record and recovery fence. The paged XIAO satellite store writes bounded entries. On XIAO the Store & Forward server suppresses immediate phone copies only with a live replay archive; a missing PSRAM archive leaves ordinary phone delivery available.

The experiment subsequently added more stock handlers and linked both T096 companion profiles. [Current fit and module results](T096-SIZE-AND-UPSTREAM-PORTS.md)

## Board measurements

Baseline `53bc60096f2f05f0d15cf1d2adba53fdf1db4f6b` is the previous PositionPrecision experiment. Candidate firmware `772b591d606a388a7d53fda376891c2ff988fef5` was built from a verified source export. Compiler binaries and resolved Zephyr configurations matched in each pair. Values are bytes.

| Board and profile | Baseline flash | Candidate flash | Flash change | Baseline static RAM | Candidate static RAM | Link result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| XIAO ESP32-S3, full private Wi-Fi/MQTT/Store & Forward | 1,447,700 | 1,448,052 | +352 | 369,632 | 369,632 | Linked |
| T096, full stock-PKI companion, LTO, BLE queue 32 | 648,412 | 650,956 | +2,544 | 297,848 | 297,848 | Exceeds flash and RAM |
| T096, RX-only comparison | 552,456 | 554,328 | +1,872 | 256,436 | 256,436 | Exceeds flash |

XIAO flash is the linked image length, SHA-256 `c5f572378c33c3ce78b5a6916360273b6f70e5d41610fb59437918efa8a657d2`. The table shows internal DRAM. IRAM stayed at 95,284 and external DRAM at 2,892,768. Hardware-dependent features are enabled according to the existing profiles. In particular, the shipped configurations exclude Audio and Paxcounter. Their optional code paths compiled separately against each board’s generated headers.

T096 has a 524,288-byte flash slot and 262,144 bytes of RAM. The full profile exceeds those limits by 126,668 bytes and 35,704 bytes respectively. RX-only exceeds flash by 30,040 bytes and has 5,708 bytes of static RAM headroom. T096 sizes use linker demand, including overflow. Both baseline T096 profiles also exceeded their slots.

The XIAO ELF shows that the branch also uses stack on demand. One Neighbor Info receive path through nested protobuf decoding accounts for at least 8,672 bytes of its 16,384-byte main stack. The capped Position path accounts for 6,272 bytes before protobuf decoding. These are sums of active function frames along individual paths. They exclude deeper calls and do not establish runtime headroom. The T096 full profile uses LTO and failed to link, so an equivalent final-ELF measurement is unavailable. The T096 RX-only build disables transmit.

## Parity results

All 19 Linux jobs passed on the measured implementation. The four independent groups compared the pinned donor with the ZT handlers in both enum ABIs. The Linux run also covered real Companion configuration and protocol paths, Store & Forward control/replay, MQTT, TAK, beacon processing and Position privacy. Each ABI passed 50 module-configuration groups, 36 stock-client telemetry checks and 19 neighbor transaction groups.

The complete extended-module catalog also passed all 32 sanitizer executions across the two enum modes. Two obsolete fixture assumptions about allowed beacon targets and reserved journal bytes were corrected after reproducing the failure on the baseline. This change affects host tests only.

The Position differential matched 2,754 encoded coordinate/precision combinations and 28 channel cases per ABI. The routing differential passed 9,105 comparisons. The production Service receive/poll tests exercise Position drop and forward behavior and 15 Neighbor Info cases per ABI. Separate checks against the real Codec2 library exercised eight frames in each of eight modes. The existing mapping translates wire mode 9 to library mode 10 for 450-bit/s audio.

Validation used native fixtures, the stock client, real libraries, and target compilers. RF delivery, audio quality, physical peripherals, runtime heap and stack high-water require hardware testing.

## Reproducing the host checks

Use a local Meshtastic firmware Git checkout containing commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. The following commands run from the ZT repository root. Set `STOCK` to that checkout and `OUT` to a fresh output directory. The harnesses compile the donor and the production adapter independently for normal and short-enum ABIs.

```sh
python3 mt_only/scripts/test-stock-background-modules.py --stock "$STOCK" --output "$OUT/background"
python3 mt_only/tests/stock_audio_pax/run.py --stock "$STOCK" --output "$OUT/audio-pax"
python3 mt_only/tests/stock_peripherals/run.py --stock "$STOCK"
python3 mt_only/tests/stock_service_modules/run.py --stock "$STOCK"
```
