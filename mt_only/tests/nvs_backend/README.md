# Zephyr NVS backend fault regression

This offline harness compiles the exact selected functions from the supplied Zephyr `settings_nvs.c`, `settings_store.c`, and optionally `nvs.c`, together with the application's real `ZephyrSettings.h` and `SettingsJournal.h`. It injects failures at mocked NVS primitive boundaries. Both settings name-cache configurations run under AddressSanitizer and UndefinedBehaviorSanitizer. The optional startup suite compiles the actual `nvs_startup` function and scripts its flash-comparison boundary.

Requirements: Python 3 and Clang with C++17, AddressSanitizer and UndefinedBehaviorSanitizer. `CXX` may select a compatible compiler. No serial ports, devices, network, flash, RF, or implementation files are accessed for writing.

From the repository root, after preparing dependencies according to the project build instructions:

```sh
python3 mt_only/tests/nvs_backend/run.py \
  --nvs mt_only/dependency-overlay/zephyr/subsys/settings/src/settings_nvs.c \
  --store /path/to/prepared/zephyr/subsys/settings/src/settings_store.c \
  --app mt_only \
  --kvss mt_only/dependency-overlay/zephyr/subsys/kvss/nvs/nvs.c
```

The default output directory is temporary and removed on exit. Add `--output /path/to/test-output` to retain generated sources, executables, logs, and a `results.json` containing input source hashes and exit codes. Compiler diagnostics may contain machine paths; do not bundle generated sources, executables, debug symbols, or compiler command logs as public evidence. The structured result deliberately omits absolute paths.

To test the actual prepared dependency copies instead of overlay sources, pass those files to `--nvs` and `--kvss`. To reproduce the original failures, use untouched pinned dependency files. With the corrected M5 overlays and write-error latch, the expected result is **25/25 checks in each cache mode plus 5/5 startup checks: 55 total**.

Coverage:

- Exact value length, missing key, and oversized read handling through the actual settings default callbacks and adapter.
- Name/value read errors cannot cause deletion, counter updates, silent absence, or a fresh journal.
- Counter read errors, truncated/oversized records, and invalid ID ranges fail initialization; actual missing counter retains fresh initialization.
- Save scans and cache lookup errors propagate before duplicate name allocation or writes.
- Genuine interrupted name/value pairs still recover, while cleanup deletion and counter-write failures propagate.
- A write that commits data and then reports failure makes the journal unavailable until reboot; a later unchanged save cannot acknowledge stale RAM state.
- Each of the three early startup sector-comparison failures aborts before recovery or mutation; ordinary erased-sector and closed/open-sector startup paths continue.

Limitations: fake NVS records are not a flash emulator. These checks verify production control flow and error propagation at documented boundaries, not physical power-loss atomicity, flash timing, garbage collection, NVS metadata integrity, full mount recovery, or hardware stack high-water marks. Startup recovery/GC functions are mocked. The selected pinned functions are extracted without rewriting their control flow; the C local identifier `delete` is renamed with a macro solely because the combined test translation unit is C++.

Repository copy whitelist (preserve relative paths):

- `run.py`
- `preamble.cpp`
- `cases.cpp`
- `startup_preamble.cpp`
- `startup_cases.cpp`
- `include/zephyr/settings/settings.h`
- `README.md`

Do not copy the rest of the review directory. Public result evidence may separately include the generated `results.json` and successful suite `.log` files after checking they contain no local paths.
