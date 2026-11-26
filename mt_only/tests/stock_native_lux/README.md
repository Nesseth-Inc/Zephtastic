# Native BH1750 and OPT3001 lux

The paired overlay binds the native BH1750 (high resolution, MTreg 69) and OPT3001 devices to the existing ZT environment channels. Both drivers fetch the device and supply photometric lux through `SENSOR_CHAN_LIGHT`. The comparison compiles the two original Zephyr channel conversions together with the pinned donor conversions and supplies every 16-bit raw word to each one.

| Device | Maximum difference across the fixture | Acquisition difference |
| --- | ---: | --- |
| BH1750 | 0.0078125 lx | Stock uses continuous high-resolution mode at the default sensitivity. Zephyr starts one high-resolution conversion per fetch and truncates its result before populating the split sensor value. |
| OPT3001 | 0.00000048 lx | Both conversions use the sensor's exponent and mantissa. Stock sets 100 ms, continuous, auto-range and latch. Native Zephyr validates both identity registers and sets continuous mode, retaining the power-on 800 ms conversion time. |

These are numerical conversion checks. A raw test word does not establish a physically valid operating point. The board fixture configures external accessories; real timing, light levels and calibration require a bench measurement. Declared native BH1750, OPT3001, TSL2561, TSL2591 and VEML7700 aliases retain their stock visible-light priority across the two generic channels and the dedicated worker. [Zephyr BH1750](https://github.com/zephyrproject-rtos/zephyr/blob/1ee3b93134be23c16ab16b9eb4183bfef63447a6/drivers/sensor/rohm/bh1750/bh1750.c) · [Zephyr OPT300x](https://github.com/zephyrproject-rtos/zephyr/blob/1ee3b93134be23c16ab16b9eb4183bfef63447a6/drivers/sensor/ti/opt300x/opt300x.c) · [Texas Instruments OPT3001 datasheet](https://www.ti.com/lit/ds/symlink/opt3001.pdf)

Supply the pinned SDK tree, the Meshtastic firmware checkout, BH1750_WE, and the original PlatformIO archive. The script checks the native source and ClosedCube archive hashes. [BH1750_WE 1.1.10](https://github.com/wollewald/BH1750_WE/tree/fcd30fea797383e5c29672549a65f97754fcd099) · [ClosedCube OPT3001 1.1.2](https://registry.platformio.org/libraries/closedcube/ClosedCube%20OPT3001)

```sh
python3 mt_only/tests/stock_native_lux/compare.py --zephyr /path/to/zephyr --stock /path/to/meshtastic --bh1750 /path/to/BH1750_WE --opt3001-archive /path/to/ClosedCubeOPT3001-1.1.2.tar.gz
```

The separately declared `zt-bh1750` provides the stock continuous mode on address 0x23. It starts the device only with enabled environment telemetry, sets MTreg 69, waits for the first complete integration, and reads subsequent results without restarting conversion. The protocol-owner test exercises the pinned original sensor and driver, feature transitions, failed transfers, and the colliding LTR553 signature. Run `stock.py --stock /path/to/meshtastic --bh1750 /path/to/BH1750_WE`.

The separately declared `zt-opt3001` uses the stock 100 ms continuous automatic range and latch. The driver verifies the TI and part IDs, waits for a completed unsaturated conversion, and powers down when the measurement setting turns off. Transfers use the selected bus at the declared stock address, 0x45 or 0x44. Use 0x45 on a T096 with a native SHT4x at 0x44. The fixture checks the Meshtastic class and ClosedCube 1.1.2 in both enum ABIs, including conflicting measurements across optical producers.

```sh
python3 mt_only/tests/stock_native_lux/stock.py --stock /path/to/meshtastic --bh1750 /path/to/BH1750_WE --opt3001-archive /path/to/ClosedCubeOPT3001-1.1.2.tar.gz
```

Among declared stock producers, each successful later visible-light reading replaces the earlier lux field in the pinned order LTR390, VEML7700, TSL2591, OPT3001, TSL2561, BH1750. UV and white light retain their own source fields. The `stock-pair.overlay` compiles both continuous protocols together on T096 or XIAO. The ordinary board configurations declare neither accessory.

The mixed overlay combines dedicated TSL2591 and VEML7700 with native OPT3001 and BH1750. The mixed tests compare both native alias orders and both enum ABIs for fetch/read errors, darkness, and independent white and UV channels through periodic and phone payloads.
