# Barometer and AHT environment telemetry

Neither the base Heltec T096 nor the base XIAO ESP32S3 has one of these sensors onboard. These fixtures exercise an explicitly attached I²C peripheral on each board. Choose one overlay set for the hardware actually fitted. On T096, apply the corresponding `t096-*-owners.overlay` before the device overlay, disabling generic probes at only the adopted addresses.

| Stock registration | Optional alias | Covered parts and measurements |
| --- | --- | --- |
| AHT10Sensor | `zt-aht` at 0x38 | AHT10/AHT20 six-byte T/RH. The stock offset on both boards is zero. Fill only missing fields. Calibration uses the status bit even when newer parts NACK the legacy command. |
| BMP085Sensor | `zt-bmp085` at 0x77 | Bosch BMP085/BMP180 (ID 0x55). Stock passes the address to the Adafruit oversampling parameter, which selects OSS 3. Two temperature conversions feed the reported temperature and the pressure correction independently. |
| DPS310Sensor | `zt-dps310` at 0x76 or 0x77 | Infineon DPS310 (ID 0x10), continuous 1 Hz/4× T/P with the factory-selected temperature source. Disabling the module retires the public cache and requests standby. SPA06 (ID 0x11) is a separate owner. |
| BMP3XXSensor | `zt-bmp3xx` at 0x76 or 0x77 | Bosch BMP388/BMP390 (ID 0x50/0x60), forced T4/P8, IIR 3, 25 Hz ODR selection, three discarded readings and Bosch floating-point compensation. A successful measurement clears relative humidity. |

`classic.overlay` combines AHT, BMP085/BMP180 and DPS310 on distinct addresses. `bmp3.overlay` tests the BMP388/BMP390 alternative. The driver rejects conflicting enabled sensor owners on the same bus/address at build time. Temporary sensor-busy periods preserve independently timestamped measurements for up to 90 seconds. Invalid frames, bus errors or invalid factory calibration suppress the affected value. Module disable clears the caches. The worker operates only on declared addresses and has bounded waits.

## Reproduce the differential

Provide the Meshtastic firmware checkout and the four Adafruit library checkouts. The runner extracts the pinned Git objects, compiles stock sensor owners and Adafruit/Bosch measurement code, and uses them as independent oracles.

```sh
python3 mt_only/tests/stock_barometers/run.py --stock /path/to/meshtastic --zephyr /path/to/zephyr \
    --aht /path/to/Adafruit_AHTX0 --bmp085 /path/to/Adafruit-BMP085-Library \
    --bmp3 /path/to/Adafruit_BMP3XX --dps /path/to/Adafruit_DPS310
```

The regression exercises 448 raw register/calibration cases and the production worker, periodic protobuf fields and an encoded, decoded phone query. It runs the AHT/BMP085/DPS and BMP3xx alternatives with the normal and short-enum ABIs under address/undefined-behavior sanitizers. Raw data and I²C responses are simulated. Physical conversion timing, pressure accuracy, bus integrity, board power require separate hardware qualification. The pinned BMP3XX singleton also computes a private altitude value. The pinned source has no reader of that value, and EnvironmentMetrics exposes pressure and temperature.

With `--zephyr`, the suite verifies the SDK source hashes and compiles its BMP180/BMP388 compensation and channel conversion using the same raw inputs. At OSS 3, BMP180 and stock differed on 89 of 112 pressure fields. BMP388 differed on 80 pressure and 107 temperature fields. These counts describe the test vectors. The pinned SDK also uses a seven-byte DHT20 read (CRC checking is configurable) and an on-demand DPS310 acquisition that defaults to 8× oversampling. Stock sensor owners and their driver versions are identified by the exact commits in `run.py`. The scoped production adapters use the Meshtastic measurement conventions.

## Linked optional fixtures

These build-only measurements used source `aa41a50de19fdf9634baa95bc5c63c80f4cb4244` and the locked Zephyr SDK and board compilers. All four configurations resolved `CONFIG_MT_STOCK_BAROMETERS=y`. Both XIAO images include paged history, public files, stock optional modules and the extended companion. Headroom uses the app partition on T096 and the first reserved data partition at 0x3b0000 on XIAO. RAM shows linked internal data RAM, including reserved stacks.

| Board and fixture | Flash used (B) | Flash free (B) | Internal RAM used (B) | Internal RAM free (B) |
| --- | ---: | ---: | ---: | ---: |
| T096 compact 680 KiB, AHT + BMP085/BMP180 + DPS310 | 652,628 | 43,692 | 253,728 | 8,416 |
| T096 compact 680 KiB, BMP388/BMP390 | 653,528 | 42,792 | 253,728 | 8,416 |
| XIAO extended, AHT + BMP085/BMP180 + DPS310 | 1,583,236 | 2,283,388 | 387,448 | 11,660 |
| XIAO extended, BMP388/BMP390 | 1,583,236 | 2,283,388 | 387,384 | 11,724 |

The two XIAO ELF files use different code and internal RAM even though image alignment produces equal binary sizes. Each reserves 2,961,568 bytes of external RAM. Physical accuracy and runtime stack/heap peaks remain hardware checks.
