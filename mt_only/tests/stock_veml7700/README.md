# VEML7700 visible and white telemetry

A `zt-veml7700` alias selects the dedicated `zephtastic,veml7700` I²C device at 0x10. The stock Meshtastic producer uses Adafruit automatic exposure for visible light, then obtains white light at the same gain and integration time. Both are transmitted in lux. The Zephyr accessory worker yields through integration delays and gates completed snapshots on the environment telemetry setting.

The donor fixture compiles the unmodified Adafruit driver and Meshtastic initialization and measurement methods. It compares gain, integration time, conversion times and both values over 66 input sequences, including signed white-count correction boundaries. It exercises real accessory-worker, periodic-report and phone-response code with bus faults, independent fields, valid darkness, an optional generic light channel and LTR390 UV coexistence. Both enum ABIs run under address and undefined-behavior sanitizers. The overlay is an external accessory build fixture. It does not establish physical presence or calibration.

```sh
python3 mt_only/tests/stock_veml7700/run.py --stock /path/to/meshtastic --adafruit /path/to/Adafruit_VEML7700
```

[Pinned Meshtastic VEML7700Sensor](https://github.com/meshtastic/firmware/blob/8e6a88d06f44cad26f1e8d7cd402939ccacb9b4c/src/modules/Telemetry/Sensor/VEML7700Sensor.cpp) · [Adafruit VEML7700 2.1.6](https://github.com/adafruit/Adafruit_VEML7700/tree/0dc00faa4313e6074a00deeb354f7099b8dd0805) · [Driver license and redistribution notice](../../docs/licenses/Adafruit-VEML7700.txt)
