# XIAO Paxcounter

The Meshtastic XIAO ESP32-S3 release at `8e6a88d0` builds Paxcounter and configures libpax at `6f52ee98`. Both normal Bluetooth and Wi-Fi must be disabled before the scanner starts. The full XIAO Zephyr profile uses the already initialized Wi-Fi driver in NULL mode and the existing Bluetooth stack. Its Wi-Fi receiver follows the driver's current country channel range at 500 ms per channel. Disabling the module releases both scan leases and restores the previous Wi-Fi filter and channel. The application leaves the station, GATT credentials, country and LoRa transmit policy under their existing owners.

Wi-Fi management and data frames and BLE observations contribute to a shared 65,536-bit period bitmap after the applicable RSSI and locally administered address checks. Only a two-byte bucket index enters the XIAO volatile queue. The phone can read aggregate Wi-Fi and BLE counts and uptime through the regular self-addressed `PAXCOUNTER_APP` empty request. The first report can have zero counts. Once a reporting period finishes, the readable counts represent that completed period. A saturated 128-entry observation queue can lose samples.

Use the pinned donor and its libpax checkout for the two-ABI receiver and counting tests.

```sh
python3 mt_only/tests/stock_paxcounter/run.py --stock "$MESHTASTIC_SOURCE" \
    --libpax "$LIBPAX_SOURCE" --output "$FRESH_OUTPUT"
```

The fixture compiles production receiver callbacks with fake Zephyr/ESP drivers. It exercises invalid ownership, each partial initialization failure, channel rotation within configured bounds, filter/channel restoration, BLE byte order and RSSI, duplicate identifiers across both receivers, and zero/nonzero interval reports. It uses synthetic frames throughout. The additional PhoneAPI fixture `phone.cpp` runs with `mt_only/scripts/test-pki-runtime.py --cases mt_only/tests/stock_paxcounter/phone.cpp` and the pinned PSA build. It checks self-directed positive, disabled and malformed requests and verifies that the mock radio receives zero transmissions.

The T096 stock Nordic base excludes Paxcounter. ZT's extended T096 profiles retain their smaller BLE-only observer. Ordinary 680 KiB release images omit it. Populated over-the-air counts and measurement accuracy require controlled physical transmitters.
