# Stock SHTXX environment sensor

Neither the base T096 nor the base XIAO ESP32S3 has an onboard SHT sensor. A board overlay can declare the external addresses actually wired on one bus. The stock-equivalent worker registers one SHTXX telemetry producer. It selects a working sensor in Sensirion v1.2.6 auto-detection order and keeps that selection across transient read failures.

| Order | Physical candidate | Stock scanner and library behavior |
| --- | --- | --- |
| 1 | SHT4x at 0x44 | Scanner separates OPT3001 using TI ID 0x5449. The library tests the one-byte SHT4x protocol first. |
| 2 | SHT20/SHT21/SHT25 at 0x40 | Scanner first handles known TI/Silergy power monitors and the ES7210. The SHT2x probe verifies both serial pages and six CRCs. The library uses fixed address 0x40. |
| 3 | SHT3x-DIS or SHT85 at 0x44 | Shared SHT3x measurement protocol. The library cannot distinguish SHT85 on this path. |
| 4 | SHT3x-DIS at 0x45 | The alternate address goes through the SHT3x protocol. The stock library does not auto-select SHT4x at 0x45. |
| 5 | SHTC1/SHTC3/SHTW1/SHTW2 at 0x70 | Shared SHTC1 measurement protocol. The driver reports a responding protocol family, without identifying a particular die. |

The Meshtastic scanner stores one result for SHTXX. Its owner calls `sht.init(bus)` without passing the scanner's winning address. The table follows that library call. The SHT2x *scanner* serial check also runs in the shared 0x41 and 0x43 cases, but the SHT2x *driver* always addresses 0x40. The analog SHT3x class has no registration in this owner.

Declare the fitted `zt-sht40`, `zt-sht44`, `zt-sht45` or `zt-sht70` slot using the example overlay. The multi-socket fixture includes all four to exercise selection. The T096 fixture also releases its deferred generic probes at the same addresses. A fixture models a bus and does not establish that every physical variant works. SHT3x/SHT4x support the three USB accuracy levels. SHT2x and the SHTC protocol use fixed accuracy. ZT returns a typed USB error for unsupported or invalid accuracy; the stock handler consumes the request even when its setter declines it. Accuracy changes take effect on subsequent conversions and survive a measurement pause in this boot. Failed measurements and disabled or expired samples withdraw the public values.

SHT2x uses separate 85 ms temperature/humidity commands, zero-seeded CRC and measurement-type bits. SHT3x and SHT4x use the selected conversion command and the stock wait time. Successful raw readings use the library's float conversion. Stock can emit an RH outside 0–100% for SHT2x and SHT4x. ZT retains its established validity range and omits those RH fields while retaining temperature in the periodic and queried reports.

## Offline verification

```sh
python3 mt_only/tests/stock_sht/run.py --stock /path/to/firmware \
    --sensirion /path/to/arduino-sht --zephyr /path/to/verified-zephyr
python3 mt_only/tests/stock_sensor_channels/run.py --stock /path/to/firmware
```

The first suite compiles Meshtastic's SHTXX owner and SHT2x serial probe at `8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c` plus Sensirion v1.2.6 at `315c1c9c009d5da5557c52572f1de13ec6d8964b`. Its donor oracle reserves two bytes for a command scratch buffer the upstream code unconditionally writes. It sends exactly the declared command length. All driver selection, measurements, arithmetic and waits come from the pinned functions. The suite covers 480 raw comparisons per ABI, all three accuracy levels, wrong devices, CRC and status failures, single-owner selection, disable/re-enable, cache expiry and an encoded phone reply.

The second suite drives the existing native Zephyr SHT3x/SHT4x accuracy Admin route and compiled patched C drivers (including periodic SHT3x rollback). Generic SHTC uses a 65536 divisor, while the donor uses 65535. Native SHT3x/4x return integer-micro-unit conversions. With `--zephyr`, the first suite compiles those three actual SDK conversion implementations. Of 96 raw pairs per placement, native SHTC differed from stock on 95 temperature and 95 RH fields. Native SHT3x and SHT4x each differed on 49 temperature and 83 RH fields. The explicit stock worker matched all reference floats. These are byte-level differences on the chosen raw inputs, not an accuracy claim. Physical bus behavior, ambient calibration and low-power wake behavior remain hardware checks.

## Linked board fixtures

The four-slot overlay linked with `CONFIG_MT_STOCK_SHT=y` and `CONFIG_MT_SHTXX_ADMIN=y` on both target toolchains at `f1c00a1240e993a607a202f358cae62a46825875`. The measurements below include the complete linked images with their respective compile-time features.

| Board profile | Flash used (B) | Flash free (B) | Internal RAM used (B) | Internal RAM free (B) |
| --- | ---: | ---: | ---: | ---: |
| T096 compact 680 KiB, fresh layout | 652,692 | 43,628 | 253,664 | 8,480 |
| XIAO extended with paged history, public files and stock optional modules | 1,583,252 | 2,283,372 | 387,152 | 11,956 |

XIAO application headroom ends at its first reserved data partition, 0x3b0000. Its external RAM allocation is 2,961,568 bytes. A separate compile-time fixture rejects an explicitly declared OPT3001 on the same bus and address at 0x44 and 0x45, and accepts the two devices on different addresses or different controllers.
