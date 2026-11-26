# Upstream Meshtastic module inventory

The source baseline is Meshtastic firmware commit `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c`. Each row names its ZT entry point. Registration and translation-unit coverage are checked against that exact Git object. A row describes the implemented path and its applicable hardware. The comparison does not measure interoperability on physical nodes.

| Upstream class or unit | Kind | Donor unit | ZT implementation | Reachable behavior |
| --- | --- | --- | --- | --- |
| `InputBroker` | registered | `InputBroker` | `src/stock/ui/SystemCommandsModule.h` | Physical input dispatch and local event policy. |
| `SystemCommandsModule` | registered | `SystemCommandsModule` | `src/stock/ui/SystemCommandsModule.h` | Local device commands use the applicable board and settings owners. |
| `BuzzerFeedbackThread` | registered | `BuzzerFeedbackThread` | `src/stock/ui/ZephyrBuzzerFeedback.h` | Input feedback requires an exported buzzer. |
| `StatusLEDModule` | registered | `StatusLEDModule` | `src/stock/peripherals/StatusLEDModule.h` | Arbitrated status and RX indication on an assigned LED. |
| `ReplyBotModule` | registered | `ReplyBotModule` | `src/stock/optional/OptionalModules.h` | Opt-in text commands and bounded responses. |
| `TrafficManagementModule` | registered | `TrafficManagementModule` | `src/stock/modules/TrafficManagementModule.h` | Filtering, suppression, rate limits, and cached NodeInfo. Over-precise Position relays are suppressed. |
| `HopScalingModule` | registered | `HopScalingModule` | `src/stock/modules/HopScalingModule.h` | Adaptive broadcast hop selection. |
| `AdminModule` | registered | `AdminModule` | `src/stock/admin/AdminModule.h` | Session, response dispatch, local administration and authenticated remote reads. See the operation limits below. |
| `NodeInfoModule` | registered | `NodeInfoModule` | `src/stock/modules/NodeInfoModule.h` | Discovery, announcements, congestion scheduling and reply decisions. |
| `MeshBeaconBroadcastModule` | registered | `MeshBeaconModule` | `src/stock/modules/MeshBeaconModule.h` | Offer and text selection and send admission. |
| `MeshBeaconListenerModule` | registered | `MeshBeaconModule` | `src/stock/modules/MeshBeaconModule.h` | Receives permitted beacon offers. |
| `MeshBeaconTxHook` | registered | `MeshBeaconModule` | `src/modules/BeaconExcursion.cpp` | Temporary channel and transmit lifecycle. |
| `PositionModule` | registered | `PositionModule` | `src/stock/modules/PositionModule.h` | Channel precision, flags, periodic and requested positions. |
| `WaypointModule` | registered | `WaypointModule` | `src/stock/waypoint/WaypointModule.h` | Origin and channel scoped waypoint history. |
| `GeofenceModule` | registered | `GeofenceModule` | `src/stock/waypoint/GeofenceModule.h` | Circle/box crossing and local notifications. |
| `TextMessageModule` | registered | `TextMessageModule` | `src/stock/message/TextMessageModule.h` | Receive, notification and display wake policy. |
| `TraceRouteModule` | registered | `TraceRouteModule` | `src/stock/modules/TraceRouteModule.h` | Path and link observations through the admitted relay. |
| `NeighborInfoModule` | registered | `NeighborInfoModule` | `src/stock/modules/NeighborInfoModule.h` | Membership, expiry, announcements and relay handling. |
| `DetectionSensorModule` | registered | `DetectionSensorModule` | `src/stock/peripherals/DetectionSensorModule.h` | GPIO edge and level events, heartbeat and message creation. |
| `AtakPluginModule` | registered | `AtakPluginModule` | `src/PublicApplication.h` | Port 78 preserves application bytes. The pinned donor receive handler returns CONTINUE. |
| `KeyVerificationModule` | registered | `KeyVerificationModule` | `src/stock/verification/KeyVerificationModule.h` | Challenge, verification code and local trust confirmation. |
| `DropzoneModule` | registered | `DropzoneModule` | `src/stock/optional/OptionalModules.h` | Opt-in radio request, ADC status and optional Lark weather. |
| `StatusMessageModule` | registered | `StatusMessageModule` | `src/stock/modules/StatusMessageModule.cpp` | Status scheduling, send and receipt. |
| `GenericThreadModule` | registered | `GenericThreadModule` | `src/stock/optional/OptionalModules.h` | Opt-in ten-second development thread. |
| `RemoteHardwareModule` | registered | `RemoteHardwareModule` | `src/stock/modules/RemoteHardwareModule.h` | GPIO wire operations on explicitly exported pins. |
| `PowerStressModule` | registered | `PowerStressModule` | `src/stock/platform/PowerStressModule.h` | Operator-enabled local USB bench scheduler and power-owner actions. |
| `CannedMessageModule` | registered | `CannedMessageModule` | `src/stock/peripherals/CannedMessageModule.h` | Selection, configured destinations and physical text input. |
| `GamesModule` | registered | `GamesModule` | `src/stock/ui/GamesModule.h` | Opt-in game selector, keyboard controls and score handling. |
| `HostMetricsModule` | registered | `HostMetrics` | `host/host_metrics.py` | Linux proc and filesystem producer publishes over local USB with stock client framing. |
| `DeviceTelemetryModule` | registered | `DeviceTelemetry` | `src/stock/telemetry/ReportModules.h` | Measured battery, uptime, utilization, local statistics and phone/radio scheduling. |
| `EnvironmentTelemetryModule` | registered | `EnvironmentTelemetry` | `src/stock/telemetry/ReportModules.h` | Environment reports, selected sensors, timestamps and scheduling. |
| `AirQualityTelemetryModule` | registered | `AirQualityTelemetry` | `src/stock/telemetry/SensorModules.h` | Local sensor and phone/radio report timing. |
| `HealthTelemetryModule` | registered | `HealthTelemetry` | `src/stock/telemetry/SensorModules.h` | Health reports for available observations and request dispatch. |
| `PowerTelemetryModule` | registered | `PowerTelemetry` | `src/stock/telemetry/ReportModules.h` | Measured voltage and current, mesh/phone reports and requested reads. |
| `SerialModule` | registered | `SerialModule` | `src/stock/peripherals/SerialModule.h` | Serial protocol, text, position and weather through the available UART. |
| `AudioModule` | registered | `AudioModule` | `src/stock/peripherals/AudioModule.h` | Codec2 audio frames, outgoing priority and optional I²S path. |
| `PaxcounterModule` | registered | `PaxcounterModule` | `src/stock/peripherals/PaxcounterModule.h` | Bounded passive observation, scheduled announcements and solicited replies. |
| `StoreForwardModule` | registered | `StoreForwardModule` | `src/stock/modules/StoreForwardModule.h` | PSRAM archive, history requests, idle heartbeat and replay. Directed router statistics and history update client information; server stats report board uptime. |
| `ExternalNotificationModule` | registered | `ExternalNotificationModule` | `src/stock/peripherals/ExternalNotificationModule.h` | Notification outputs, sound, dismissal and repeat policy. |
| `RangeTestModule` | registered | `RangeTestModule` | `src/stock/modules/RangeTestModule.h` | Range packets, distance and optional mounted-filesystem CSV. |
| `WatchdogThread` | registered | `WatchdogThread` | `src/stock/platform/ZephyrWatchdog.cpp` | Optional external watchdog pulse via two declared GPIOs. The shipping boards use their internal watchdog. |
| `RoutingModule` | registered | `RoutingModule` | `src/stock/routing/RoutingModule.h` | Routing control, relay admission, acknowledgements and hop rules. |
| `OnScreenKeyboardModule` | companion | `OnScreenKeyboardModule` | `src/stock/ui/OnScreenKeyboardModule.h` | Stock keyboard state via the ZT display owner. |
| `ReplyModule` | companion | `ReplyModule` | `src/stock/optional/OptionalModules.h` | Opt-in example responder. Its registration is commented out in the donor. |
| `UnitConversions` | support | `UnitConversions` | `src/stock/optional/UnitConversions.h` | Wire and display conversions shared by the weather adapter. |
| `Breakout` | support | `Breakout` | `src/stock/ui/games/Breakout.cpp` | Optional game core. |
| `ChirpyRunner` | support | `ChirpyRunner` | `src/stock/ui/games/ChirpyRunner.cpp` | Optional game core. |
| `Snake` | support | `Snake` | `src/stock/ui/games/Snake.cpp` | Optional game core. |
| `Tetris` | support | `Tetris` | `src/stock/ui/games/Tetris.cpp` | Optional game core. |
| `MQTT` | support | `MQTT` | `src/stock/mqtt/MQTT.h` | Separate upstream service. Published mesh traffic uses the broker or companion proxy. |
| `AmbientLightingThread` | support | `AmbientLightingThread` | `src/stock/peripherals/AmbientLightingThread.h` | Separate RGB output thread with board-selected capabilities. |
| `PositionPrecision` | support | `PositionPrecision` | `src/stock/position/PositionPrecision.cpp` | Shared wire precision in the configured channel. |

## What the inventory covers

The checker reads the pinned Git objects for `src/modules/Modules.cpp` and every translation unit under `src/modules`. It removes C++ comments before identifying constructors and checks each documented ZT implementation exists. It also matches every sensor filename against the hardware table and rejects duplicate, missing and unexpected sensor rows. The 42 live registrations include the input broker, buzzer, watchdog and three separate MeshBeacon objects. The 44 module translation units include the four games, the unused Reply example, OnScreenKeyboard and UnitConversions. MQTT, AmbientLighting and PositionPrecision are listed because other firmware owners construct or use them. The pinned tree contains no third-party modules in `src/modules/optional/`.

The 46 translation units under `src/modules/Telemetry/Sensor/` are hardware backends and their common infrastructure. Board devicetree determines which sensors can produce measurements. An unpopulated board returns no reading. Report timestamps come from the node clock and readings retain the applicable Meshtastic units.

## Reproducing the tests and opt-in builds

```sh
python3 mt_only/scripts/check-upstream-module-inventory.py --stock "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/stock_platform_modules/notice_encoding.py
python3 mt_only/tests/stock_platform_modules/descriptor_layout.py
python3 mt_only/tests/stock_platform_modules/run.py --stock "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/stock_platform_modules/host_metrics.py --stock "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/stock_optional/run.py --stock "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/stock_ui/run.py "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/stock_admin_routing/run.py --stock "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/stock_l76k/run.py --stock "$MESHTASTIC_FIRMWARE"
python3 mt_only/tests/run-store-forward-companion.py
python3 mt_only/tests/stock_sensor_channels/run.py --stock "$MESHTASTIC_FIRMWARE" --maxim "$SPARKFUN_MAX3010X"
```

The Linux host test uses the Meshtastic Python package. On a Linux host with local USB access, `python3 mt_only/host/host_metrics.py --device /dev/ttyACM0 --interval-minutes 5 --channel 0` enables a five-minute report. The default interval is zero. The firmware receives this variant over its connected USB companion. Info logs from the GenericThread and PowerStress examples use the USB companion's idle queue.

Use the repository's normal board build arguments and append `--stock-optional-modules` to enable the reply examples, Dropzone, GenericThread, USB host metrics and local PowerStress. T096 also selects the games. Use `--extra-dtc-overlay` for declared accessory hardware. The two fixtures in `tests/stock_sensor_channels/scd41.overlay` and `tests/stock_platform_modules/watchdog.overlay` exercise additional drivers at build time.


## Module behavior and remaining differences

- The Service handles ReplyBot, Reply and Dropzone once per admitted RF packet even with the USB console disabled. Channel validation, signed-frame admission, PKI replay checks and source policy run before a reply. The firmware ignores MQTT ingress for these autonomous responses.
- The NodeDB journal retains next-hop hints and reloads older journal versions. A recovered hint takes effect after its relay is observed again without an ID collision. Config reset retires contacts, clears application preferences and preserves the identity, nonce history, FLASH backup and BLE bonds. Full-device reset replaces the identity on the XIAO and on T096 fresh profiles with a dedicated retirement page. Local file transfer uses the application-owned file or explicit public directory. [Admin operation matrix](UPSTREAM-ADMIN-OPERATIONS.md)
- The sensor overlay brings up the SCD41, DS2482/DS18B20 and MAX30102 code paths at build time. The 100-sample optical algorithm has a pinned synthetic reference. Board drivers, calibration commands and physical accuracy depend on the installed accessory. The stock boards use ZT's compact telemetry UI. [Sensor bindings, measurements and donor tests](../tests/stock_sensor_channels/README.md)
- The PowerStress example accepts commands on the connected local USB owner with the operator's power-monitor setting enabled. Bluetooth changes use the board policy, timed sleep uses the quiescence and wake checks, and the busy interval yields for housekeeping and the watchdog. The donor's marker-only opcodes remain markers. The ATAK plugin preserves the application's payload unchanged, as does its pinned receive handler.
- The T096 retains 64 opaque routing-history slots and eight outbound BLE packets. The compact build additionally uses checked nanopb descriptor widths, memory-backed streams, a single application LTO partition, a zero-initialized observation queue and byte-pair compression of companion notice text. Native comparisons check every descriptor field and every truncation length of the packed notices.

## Companion behavior on the experiment

The XIAO retains the last broadcast time for NodeInfo, Position, MeshBeacon and five telemetry producers. A local PhoneAPI heartbeat with nonce 1 requests a rate-limited NodeInfo broadcast on the primary channel. Ordinary heartbeats retain their queue-status response. [Broadcast cooldowns and donor comparisons](../tests/stock_transmit_history/README.md)

The ordinary handshake enumerates thin peer NodeInfo records, then replays eligible public Position, DeviceMetrics, EnvironmentMetrics and Status samples after substream completion. Config-only and nodes-only nonces follow the donor paths. Reconnect preserves reported position precision, including the stock 32-bit fallback for older peers. Public and direct traffic are kept in distinct replay domains. The private filesystem profile can recover eligible samples after a cold boot, bound to the local identity, channel settings, and peer enrollment. [Cold-boot persistence](../tests/stock_satellite_history/README.md)

Waypoints and subscriptions persist in a bounded ten-entry journal. The first observation establishes a geofence baseline. The firmware tracks up to 256 node/waypoint crossing pairs. Changing unrelated display or device preferences preserves the channel generation across reboot. Changing channel identities and returning to an earlier configuration invalidates stale records.

All numeric device roles have a handling path, including SENSOR, TRACKER, LOST_AND_FOUND and CLIENT_HIDDEN. Role 10 requires its build option. Activity and position gates are verified at the real role setter and scheduler. Public files can be listed, uploaded and downloaded over local USB or an eligible encrypted BLE connection. [Role comparisons](../tests/stock_operating_roles/run.py) · [Phone file tests](../tests/stock_phone_files/README.md)

### XIAO browser messaging

With `--xiao-full-companion` and an enabled, saved Wi-Fi station configuration, open `http://<XIAO-IPv4-address>/` from the joined network. The XIAO serves the page and its assets. Choose an active channel, leave the recipient blank for broadcast, or enter an eight-digit node ID for a directed message. The usual radio TX and private-recipient checks still apply. The page distinguishes submission from a radio acknowledgement.

The HTTP service shares one companion connection with port 4403. It listens on the joined station's IPv4 address, accepts its local subnet, checks the browser origin, and binds the browser session to a random cookie and peer address. The browser can read text and delivery status through `GET /api/v1/fromradio` and submit text through `PUT /api/v1/toradio`. Its configuration handshake returns public node and channel information. File transfers, device Admin, credentials and provisioning are excluded from the browser interface. The page uses HTTP on port 80. A Meshtastic client that connects directly to TCP port 4403 uses the existing companion Admin policy. [Browser fixture](../tests/stock_browser_page) · [Transport and permission tests](../tests/browser_messaging_tests.cpp)

### Module behavior with stock limits

The full display profiles show Environment, Power, Air Quality and Health, including the selected source and age when available. Environment offers Mesh, Local and Favorite views. The T096 presets include these detailed frames on the 728 and 732 KiB layouts. The XIAO uses them when the expansion OLED is declared. [Display coverage](../tests/stock_report_frames)

Both boards accept the stock three-entry LoRa `ignore_incoming` list over USB or BLE. The setting persists. Matching RF and MQTT senders are filtered before routing and phone delivery. Channel identity and locally originated transmissions keep their separate semantics. [Sender filter and storage compatibility](../tests/stock_lora_ignore/run.py)

The phone directory holds 120 entries including self on T096 and 200 on XIAO. The full paged XIAO companion supports the stock 198 protected remote peers. A local edit requesting the 15th protected peer moves the public NodeDB to the existing private filesystem. RF discovery alone keeps the legacy journal. Removing enough records to reach at most 14 protected and 16 total remote peers writes both inline selectors for older firmware. T096 and smaller profiles retain 14 protected peers on a 16-entry working cache; stock T096 supports 118. The ordinary XIAO and T096 persist 16 private contacts. The separately signed XIAO profile can import them and expand to 198 trusted private contacts after local USB activation. Both XIAO profiles retain four rotating active sessions. [Expanded contact storage and recovery](XIAO-EXPANDED-CONTACTS.md). [Protected NodeDB migration and rollback](../tests/stock_protected_nodes/README.md) · [Directory tests](../tests/stock_peer_directory/cases.cpp) · [Private contacts and replay](../tests/contact_cache_cases.cpp)

ZT exposes 25 active sub-GHz regional plans plus UNSET, and its channel policy suppresses over-precise Position relays. The remaining display preferences, radio configurations and remote Admin operations have board- or policy-specific limits. [Directory behavior](../tests/stock_peer_directory) · [Admin operations](UPSTREAM-ADMIN-OPERATIONS.md)

## Full companion images on T096 and XIAO

The table measures firmware `8af2faefc28a4e3d37d02420109fd21be8d73c69`. T096 flash ends at its selected application partition. XIAO flash ends at its first data partition at `0x3b0000`. RAM includes statically reserved stacks and heaps. All values are bytes.

| Profile | Flash used | Flash free | Static RAM used | Static RAM free |
| --- | ---: | ---: | ---: | ---: |
| T096 680 KiB initial-release | 652,392 | 43,928 | 249,504 | 12,640 |
| T096 680 KiB extended, reset and games | 695,956 | 364 | 257,184 | 4,960 |
| T096 728 KiB extended, public files and history | 742,944 | 2,528 | 260,508 | 1,636 |
| T096 732 KiB extended, public files and paged history | 747,960 | 1,608 | 261,020 | 1,124 |
| XIAO S3 extended, Wi-Fi, MQTT, Store and Forward, public files and paged history | 1,530,004 | 2,336,620 | 381,256 | 17,852 |
| XIAO S3 extended with the expansion OLED, keyboard and games | 1,606,356 | 2,260,268 | 385,712 | 13,396 |

On the paged T096, initializing the empty range log and peer-directory caches at startup saved 4,056 and 2,304 flash bytes respectively, preserving their capacities. Role projection and settings-journal validation now share a temporary buffer under the preferences lock. This saved another 1,024 bytes of static RAM on both full T096 layouts. The paged image reserves 41,680 bytes for the mesh service, 17,480 for private identity startup, 16,460 for the system heap and 16,448 for the main stack and its guard. Runtime stack and heap high-water marks still need measurement on hardware. [Role and journal recovery tests](../tests/remote_role_workspace/run.py)

Use `--t096-full-companion` for the 728 KiB layout with a 76 KiB filesystem, or `--t096-paged-companion` for the 732 KiB layout with a 72 KiB filesystem and larger per-kind report history. The existing 512 KiB legacy layout and 680 KiB profile remain available. All fresh profiles preserve 64 routing-history entries and eight outbound BLE packets. One fresh identity with all 16 private contacts populated used 1,632 live PKI value bytes in the shared 16 KiB NVS partition. Preserve a verified full-device backup and check the original flash readback before a future fresh installation. [Partition map and offline checker](T096-FRESH-DOMAIN.md)

The XIAO S3 uses the headless profile unless the expansion panel is declared. Add `--xiao-expansion-oled` to the full XIAO companion to select its 128×64 SSD1306, keyboard and games. Both builds keep 64 outbound BLE packets. The panel uses the board I²C connector and the firmware keeps the display transfer buffer in internal RAM. [Expansion board pinout](https://wiki.seeedstudio.com/Seeeduino-XIAO-Expansion-Board/)

The official Meshtastic 2.8.1 T096 ELF uses 708,684 bytes in an 802,816-byte application region, leaving **94,132 bytes of flash**. Its initialized data and BSS use 105,092 bytes. The linker reserves 138,620 bytes between `__HeapBase` and `__HeapLimit` and 2,048 bytes for the main stack. This is separate from Zephyr's static accounting, which already includes its reserved heaps, queues and thread stacks. Neither ELF measures runtime peak use. The stock ELF SHA-256 is `78a068aa126570445a14bf80aa936ddb32bc860c39ded170d080ca079cf459a7`. [Stock 2.8.1 release](https://github.com/meshtastic/firmware/releases/tag/v2.8.1.8e6a88d)

The inventory covers 42 registrations, 44 module translation units and 46 sensor backend files at the pinned upstream revision. Stock radio interoperability, real BLE load, powered storage recovery, accessory calibration, and stack and heap high-water marks await hardware testing. The compact local display, remote Admin permissions and bounded private contact store have documented differences from stock. Over-precise Position relays are suppressed according to the selected channel policy. [Admin permissions](UPSTREAM-ADMIN-OPERATIONS.md) · [Sensor-specific evidence](UPSTREAM-TELEMETRY-SENSORS.md)

## Earlier linked companion images with persistent peer history

The table compares firmware at `e00d0f0a` and the named XIAO extended preset at `e463ad2e`. All values are bytes. T096 flash ends at the application partition. XIAO flash ends at its first reserved data partition, address `0x3b0000`. RAM includes reserved stacks. Runtime high-water marks require a board measurement.

| Profile | Flash used | Flash free | Static RAM used | Static RAM free |
| --- | ---: | ---: | ---: | ---: |
| T096 680 KiB, initial release | 651,880 | 44,440 | 250,912 | 11,232 |
| T096 680 KiB, extended onboard | 677,940 | 18,380 | 256,032 | 6,112 |
| T096 680 KiB, extended with device reset, optional modules and games | 695,404 | 916 | 258,592 | 3,552 |
| T096 728 KiB, extended with reset, optional modules, games, peer history and public files | 744,472 | 1,000 | 261,212 | 932 |
| XIAO initial release with Wi-Fi, MQTT and Store & Forward | 1,445,732 | 2,420,892 | 369,864 | 29,244 |
| XIAO initial release with those services, optional modules, peer history and public files | 1,515,940 | 2,350,684 | 378,952 | 20,156 |
| XIAO extended with those services, optional modules, peer history and public files | 1,516,692 | 2,349,932 | 380,608 | 18,500 |

The separate 728 KiB T096 preset is `--t096-full-companion`. Its main stack reservation is 16,384 bytes. The static compiler report gives 3,072 bytes for `main`, 3,352 for the thin-directory save function and 2,056 for the warm-directory worker step. The 680 KiB initial-release combination with both file services used 696,472 bytes of application flash in the size report, 152 bytes beyond its slot. [T096 layout and offline enrollment check](T096-FRESH-DOMAIN.md)

The XIAO extended preset is `--xiao-full-companion`. It selects the three additional onboard options used on the extended T096, including passive BLE observation. The matched initial-release file-services row uses 752 fewer flash bytes and 1,656 fewer internal RAM bytes. Pin-dependent operations still require a declared accessory. Both XIAO profiles use a 64-packet BLE output queue.

On simulated NOR the 19-sector T096 history volume held 119 thin remote peer entries, 100 warm peers, two public files and repeated atomic replacements of a 7,368-byte history file. The occupied block count rose from 12 to 14 after writing the history. Composed tests used both enum ABIs and covered interrupted saves, recovery after late mount, rekey and removal, and data erasure during reset.

## Earlier experiment measurements

The board images were built from `0fde6802cfc1d8aad34bffdfd07fc4dbbe48109d`. Both T096 profiles use the 696,320-byte fresh-domain application slot and 262,144 bytes of SRAM. XIAO RAM in the table is internal data RAM. The figures are bytes from the final image and linker reports.

| Board and profile | Flash used | Flash free | Static RAM used | Static RAM free |
| --- | ---: | ---: | ---: | ---: |
| T096 extended onboard | 680,500 | 15,820 | 258,208 | 3,936 |
| T096 extended onboard with opt-in modules and four games | 693,548 | 2,772 | 260,640 | 1,504 |
| T096 initial-release companion | 654,168 | 42,152 | 253,024 | 9,120 |
| T096 initial-release with opt-in modules and four games | 667,088 | 29,232 | 255,520 | 6,624 |
| XIAO Wi-Fi/MQTT and Store & Forward | 1,445,140 | 6,943,212 | 369,672 | 29,436 |
| XIAO Wi-Fi/MQTT and Store & Forward with opt-in modules | 1,445,620 | 6,942,732 | 369,744 | 29,364 |
| T096 initial-release with the test sensor and watchdog overlay | 665,592 | 30,728 | 259,296 | 2,848 |

Compared with the preceding experiment, the extended T096 saves 5,032 flash bytes and 132 static RAM bytes. The initial-release T096 saves 1,664 flash bytes and uses 380 additional static RAM bytes. The default XIAO image saves 3,040 flash bytes and uses 40 additional bytes of internal data RAM. All comparisons use the same compiler toolchains. On XIAO the default image uses 95,284 bytes of instruction RAM and 2,894,432 bytes of external RAM.

All five T096 UF2 images were checked against HEX and BIN, including the nRF52840 family ID and the application boundaries. They require the fresh-domain storage layout. The application overlaps the old identity's 168 KiB PKI journal. The current experiment provides no migration for that history. [Fresh-domain layout and enrollment](T096-FRESH-DOMAIN.md)

56 Linux regression jobs passed on `e1759972` after the NVS fixtures were updated to compile Zephyr’s settings deletion function. That commit has the same production sources as the board images. Another 8 Linux jobs ran with the T096 memory-stream nanopb options. The extended-module suite passed 32 sanitizer executions. The routing differential passed 9,112 comparisons. Position precision passed 2,754 wire cases and 28 channel-setting cases per ABI. The stock game cores passed 21 donor tests. Keyboard checks covered 88 actions, 5,000 cursor moves, 147 emote labels and 104 bitmaps. The optional responder tests cover standalone RF ingress with no phone, MQTT exclusion and authenticated private replies with synthetic PSA keys. The sensor suite checks both enum ABIs against its pinned sources, including a synthetic optical result of 75 BPM and 91% SpO₂.

These measurements establish linked image size and fixture behavior. Physical radio interoperability, actual BLE pairing under load, optical accuracy, runtime stack/heap high-water marks and field recovery require board tests.

### XIAO BLE LogRadio

The XIAO companion exposes the stock LogRadio read/notify UUID
`5a3d6e49-06e6-4423-9944-e9de8cdf9547`. Stock
`RedirectablePrint::log_to_ble` encodes a bare `meshtastic.LogRecord`.
The XIAO uses the same protobuf for its allowlisted events when
`security.debug_log_api_enabled` is active and the BLE session satisfies the configured
encryption policy. A LogRadio-only client can subscribe directly. The last value
supports ATT Read Blob. Disabling logging or closing the session clears retained values.
A congested subscriber can miss events. The T096 retains its existing PhoneAPI log
stream. The native checks are in `mt_only/tests/stock_log_radio/run.py`.
