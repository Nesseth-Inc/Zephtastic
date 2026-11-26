# T096 first-install layout regression

Run `python3 mt_only/tests/stock_t096_layout/run.py` from the repository root. The fixture uses temporary, synthetic 1 MiB flash images and tests the production offline checker and reset-floor reader. It requires Python 3 and a C++17 compiler.

The tests contaminate the first and last byte of every 4 KiB page in the former PKI, settings and LittleFS areas, including the independent retirement page. They also exercise missing UF2 data, truncated and oversized files, occupied application bytes outside the guarded range, the original fresh-only profile and interrupted floor enrollment. The mocked flash driver verifies every read address and denies writes.

Pass `--full-zephyr <build>/zephyr` to check the actual generated 728 KiB devicetree and `.config`. Pass `--original-zephyr <build>/zephyr` for the original 680 KiB reset-capable profile. With `--linked`, the runner verifies those builds’ ELF, Intel HEX, BIN and UF2 files, including physical addresses, programmed bytes, alignment padding, family ID and application headroom.

An offline first-install pass establishes that the specified regions in the supplied image are erased and the boot reservation contains data. Physical board identity and bootloader authenticity require separate evidence.
