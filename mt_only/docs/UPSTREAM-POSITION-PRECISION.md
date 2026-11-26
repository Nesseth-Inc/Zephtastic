# Stock PositionPrecision on Zephtastic

The experiment ports the complete Meshtastic 2.8.1 position-precision module to ZT. Its channel functions receive a view of the caller’s actual `Config`, so the same code works with active, staged, and restored settings. The port keeps the existing sharing consent, fixed-position/GNSS availability checks, and selected transmit slot. It caps publicly decryptable channels at 15 bits and applies the existing opt-in event-key exclusion. ZT also caps a shortened stored key that pads to a member of the public key family. The packet adapter checks the declared input length and distinguishes an empty encoded position from an encode failure. [Stock source](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/mesh/PositionPrecision.cpp) · [ZT adapter](../src/stock/position/PositionChannels.h)

## Target builds

Baseline `5276207590a6ca5a6e574dd88356438cac03d25f` and implementation `673a8e47bc202061e217a85de437c9d6ca353060` were built from verified source exports. Each pair used clean build directories, the same Zephyr SDK 1.0.1, dependencies, overlays, compiler flags and build timestamp. Each pair’s resolved Zephyr configuration was byte-identical. Values are bytes.

| Board and profile | Flash before | Flash after | Change | Static RAM before | Static RAM after | Build result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| XIAO ESP32-S3, full private Wi-Fi/MQTT/Store & Forward | 1,447,620 | 1,447,700 | +80 | 369,632 | 369,632 | Both images linked |
| T096, full stock-PKI companion, LTO, BLE queue 32 | 648,332 | 648,412 | +80 | 297,848 | 297,848 | Both exceeded flash and RAM |
| T096, RX-only comparison | 552,296 | 552,456 | +160 | 256,436 | 256,436 | Both exceeded flash |

XIAO flash is the verified image length. RAM is the linker's internal DRAM region, with 29,476 bytes remaining. On the path into channel selection, XIAO disassembly shows a 144-byte lookup frame in the baseline. The port uses consecutive 48- and 144-byte frames before calling the same channel-selection routine, adding 48 bytes on that call path. Runtime stack high-water and heap usage were not measured.

T096 has a 524,288-byte application slot and 262,144 bytes of RAM. The full-profile trial exceeds the slot by 124,124 bytes and RAM by 35,704 bytes. Flash demand comes from the linker overflow and RAM from the span between RAM origin and `_image_ram_end` in the linker map. Neither T096 comparison produced an installable binary.

## Parity checks

The independently compiled upstream source and ZT produced identical encoded positions for **2,754 coordinate/precision combinations** in each of the normal and short-enum ABIs. In each ABI, 28 further cases compared the upstream channel view with real ZT settings. Ten sanitised regression groups passed in each combination of the two ABIs and the normal/event-key builds. They cover public aliases and inherited keys, private channels, absent consent, event-key matching, packet redaction and malformed lengths. [Checked-in regression suite](../scripts/test-channel-privacy.py)

Linux integration tests exercised fixed-position settings, position queries and replies, and channel privacy through the client transport. The stock Meshtastic Python client (2.7.11) passed the fixed-position and query suites against ZT in both enum modes. The position scheduler matched the pinned stock oracle in all seven scenario groups in each mode.

The native GNSS parser and position/heading projection passed in both enum modes. Two GNSS harnesses also failed on the unchanged baseline. One still requires Config version 19, while both builds use version 23. The other fails a USB diagnostic counter check in its companion fixture. The experiment used host transports and board builds; no radio was flashed.
