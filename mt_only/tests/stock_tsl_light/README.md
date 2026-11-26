# TSL2591 and TSL2561 lux

The explicit `zt-tsl2591` and `zt-tsl2561` aliases select independent devices on the shared accessory worker. Both stock sensor classes use address 0x29. A board supplying both must wire them to distinct I²C controllers. The shared worker validates the chip, applies 1× gain and waits 120 ms after enabling each integration. It reads broadband before IR, calculates lux and powers down. A failed transfer suppresses the affected measurement. The firmware preserves the TSL2561 stock saturation signal of 65536 lux. It omits negative overflow and non-finite TSL2591 results from its finite-valued telemetry.

Existing `zt-environment` aliases targeting a native Zephyr TSL2591 use the driver's paired `SENSOR_CHAN_ALL` lux conversion. Its LIGHT and IR getters report relative channel strengths. The native TSL2561 driver's configurable photometric output remains available with its configured timing and gain. Stock uses T/FN/CL package coefficients and 101 ms, while the native defaults are 402 ms and 16× and use a different saturation policy.

The fixture compiles both pinned stock sensor classes and their Adafruit drivers. On each enum ABI it checks 400 ratio, darkness, overflow and saturation cases per model. It also checks the normal environment worker, phone protobuf, failure/disable and the native channel selection. Overlays describe external accessories and support build validation. Physical population, calibration and timing on a real bus remain unverified.

```sh
python3 mt_only/tests/stock_tsl_light/run.py --stock /path/to/meshtastic --tsl2591 /path/to/Adafruit_TSL2591_Library --tsl2561 /path/to/Adafruit_TSL2561
```

[Pinned TSL2591 donor](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/modules/Telemetry/Sensor/TSL2591Sensor.cpp) · [Pinned TSL2561 donor](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/modules/Telemetry/Sensor/TSL2561Sensor.cpp) · [Adafruit TSL2591 1.4.5](https://github.com/adafruit/Adafruit_TSL2591_Library/tree/a857bf93c10b5d60d02928c7a6c364e67ff5e0f3) · [Adafruit TSL2561 1.1.3](https://github.com/adafruit/Adafruit_TSL2561/tree/bf2adc60e22a46356d48755931852d2d6d0d2238) · [Driver notices](../../docs/licenses/Adafruit-TSL-light.txt)
